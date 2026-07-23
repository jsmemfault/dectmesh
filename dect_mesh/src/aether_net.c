/*
 * /net/aether — a Plan 9 /net/udp-style reliable-datagram interface to the
 * Æther mesh, as a custom 9P fs_ops. Conforms to doc/NET_AETHER_SPEC.md.
 *
 * This is the DECT-optimized server: reliability (acks/retries/dedup/ordering)
 * is provided below the filesystem by aether_mesh_send_reliable() + the DECT
 * NR+ PHY, so this layer is just the conversation state machine + the 9P tree.
 * The fs contract is message-in / message-out (spec §6).
 *
 *   /net/aether/
 *       clone            open/walk -> allocate a conversation; read -> "N"
 *       status           "<active>/<max>" + neighbour summary
 *       addr             this node's 6-byte mesh address (colon-hex)
 *       stats            stats(5)-style datagram counters (sent/rcvd/...)
 *       maxmsg           reassembled-datagram ceiling in bytes (spec §6)
 *       <N>/             one conversation (N = 0 .. MAX_CONNS-1)
 *           ctl          write: connect <addr> | announce | hangup ; read "N\n"
 *           data         one datagram per read/write (read blocks); connected ->
 *                        bare payload, announced -> [src]-prefixed read /
 *                        [dst]-prefixed write
 *           local        local address
 *           remote       peer address, or empty if unbound
 *           status       connected <addr> reliable | announced | unconnected
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/net/heymac.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* A best-effort broadcast is one-shot at the mesh layer, so a transient busy
 * channel (LBT -EBUSY) would surface to the client as a send failure. Absorb it
 * here with a short jittered busy-retry (backlog #3: transport tolerates
 * transient RF). Reliable sends get the equivalent in aether_route.c. */
#define NET_BCAST_BUSY_RETRIES    6
#define NET_BCAST_BUSY_BACKOFF_MS 15

LOG_MODULE_REGISTER(aether_net, LOG_LEVEL_INF);

#define AETHER_MAX_CONNS   4
/* One datagram = one DECT frame payload. Track the mesh payload cap so the
 * /net/aether write limit and the rxq buffers match what aether_mesh_send can
 * actually carry (no "9P accepts it but the mesh rejects it" gap). DECT-sized,
 * not the old 512 LoRa-era value. */
#define AETHER_MAX_MSG     CONFIG_AETHER_MAX_PAYLOAD
/* Per-conversation RX datagram queue. Was 4 -- too shallow: under chat load the
 * mesh delivers datagrams faster than the 9P client drains them (the 9P framer/
 * async-read path is preempted by the COOP(2) mesh workq), so the queue filled at
 * 4 and dropped ("conv N rxq full, dropping datagram"), degrading delivery. 16
 * absorbs realistic bursts while the client catches up in the mesh-idle gaps. */
#define AETHER_RXQ_DEPTH   16
#define AETHER_NET_RETRIES 3     /* reliable-send retry budget (DECT PHY also helps) */

/* One received datagram queued for a conversation's data reader. */
struct aether_dgram {
	uint8_t src[6];
	uint16_t len;                      /* 0 => hangup sentinel (EOF) */
	uint8_t data[AETHER_MAX_MSG];
};

/* CONV_BCAST: spec §6a best-effort broadcast party line (connect ff:ff:ff:ff:ff:ff). */
enum conv_state { CONV_UNCONNECTED = 0, CONV_CONNECTED, CONV_ANNOUNCED, CONV_BCAST };

/* node "kind" so an fs_ops callback can dispatch from the bare node. Stored in
 * ninep_fs_node.data. */
enum anode_kind {
	K_ROOT = 0, K_CLONE, K_TOPSTATUS, K_ADDR, K_STATS, K_MAXMSG,
	K_CONVDIR, K_CTL, K_DATA, K_LOCAL, K_REMOTE, K_CSTATUS,
};

struct anode {
	enum anode_kind kind;
	struct aether_conv *conv;          /* NULL for the top-level nodes */
};

struct aether_conv {
	bool in_use;
	int slot;
	enum conv_state state;
	uint8_t peer[6];                   /* valid when CONNECTED */

	struct k_msgq rxq;
	char rxq_storage[AETHER_RXQ_DEPTH * sizeof(struct aether_dgram)];

	/* the <N>/ subtree, statically owned by the slot */
	struct ninep_fs_node dir, ctl, data, local, remote, status;
	struct anode an_dir, an_ctl, an_data, an_local, an_remote, an_status;
};

struct aether_net_fs {
	struct net_if *iface;
	uint8_t myaddr[6];
	uint32_t next_qid;

	struct ninep_fs_node root, clone, topstatus, addr, statf, maxmsgf;
	struct anode an_root, an_clone, an_topstatus, an_addr, an_statf, an_maxmsgf;

	/* stats(5)-style datagram counters, surfaced at /net/aether/stats. Each
	 * field is touched from one context (tx_* from the writer thread, rx_*
	 * from the mesh RX callback), so plain uint32_t increments suffice. */
	struct {
		uint32_t tx, tx_bytes, tx_err;
		uint32_t rx, rx_bytes, rx_drop;
	} ctr;

	struct aether_conv convs[AETHER_MAX_CONNS];
	struct k_mutex lock;
};

static struct aether_net_fs g_fs;

/* ---- helpers ---- */

static inline struct anode *AN(struct ninep_fs_node *n) { return n->data; }

static int fmt_addr(char *s, size_t sz, const uint8_t a[6])
{
	return snprintf(s, sz, "%02x:%02x:%02x:%02x:%02x:%02x",
			a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* This node's durable identity (node_eui) -- the address /net/aether exposes.
 * Read live from the mesh ctx so it reflects the boot identity, NOT the HONR
 * routing address (which churns with the tree). Falls back to the cached value
 * if the mesh isn't up yet. */
extern struct aether_mesh_ctx *g_mesh_ctx;
static const uint8_t *my_identity(void)
{
	return g_mesh_ctx ? g_mesh_ctx->node_eui : g_fs.myaddr;
}

/* Parse "aa:bb:cc:dd:ee:ff" -> 6 bytes. Returns 0 on success. */
static int parse_addr(const char *s, uint8_t out[6])
{
	unsigned int b[6];

	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
		return -EINVAL;
	}
	for (int i = 0; i < 6; i++) {
		if (b[i] > 0xff) {
			return -EINVAL;
		}
		out[i] = (uint8_t)b[i];
	}
	return 0;
}

static void node_init(struct ninep_fs_node *n, const char *name,
		      enum ninep_node_type type, struct anode *an,
		      enum anode_kind kind, struct aether_conv *conv)
{
	memset(n, 0, sizeof(*n));
	strncpy(n->name, name, sizeof(n->name) - 1);
	n->type = type;
	n->mode = (type == NINEP_NODE_DIR) ? (0755 | NINEP_DMDIR) : 0666;
	n->qid.path = g_fs.next_qid++;
	n->qid.type = (type == NINEP_NODE_DIR) ? NINEP_QTDIR : NINEP_QTFILE;
	an->kind = kind;
	an->conv = conv;
	n->data = an;
}

static void link_child(struct ninep_fs_node *parent, struct ninep_fs_node *child)
{
	child->parent = parent;
	child->next_sibling = parent->children;
	parent->children = child;
}

static void unlink_child(struct ninep_fs_node *parent, struct ninep_fs_node *child)
{
	struct ninep_fs_node **pp = &parent->children;

	while (*pp) {
		if (*pp == child) {
			*pp = child->next_sibling;
			child->next_sibling = NULL;
			return;
		}
		pp = &(*pp)->next_sibling;
	}
}

/* Build a conversation's <N>/ node subtree and link <N> under the root. */
static void conv_build_nodes(struct aether_conv *c)
{
	char nm[8];

	snprintf(nm, sizeof(nm), "%d", c->slot);
	node_init(&c->dir, nm, NINEP_NODE_DIR, &c->an_dir, K_CONVDIR, c);
	node_init(&c->ctl, "ctl", NINEP_NODE_FILE, &c->an_ctl, K_CTL, c);
	node_init(&c->data, "data", NINEP_NODE_FILE, &c->an_data, K_DATA, c);
	node_init(&c->local, "local", NINEP_NODE_FILE, &c->an_local, K_LOCAL, c);
	node_init(&c->remote, "remote", NINEP_NODE_FILE, &c->an_remote, K_REMOTE, c);
	node_init(&c->status, "status", NINEP_NODE_FILE, &c->an_status, K_CSTATUS, c);
	link_child(&c->dir, &c->ctl);
	link_child(&c->dir, &c->data);
	link_child(&c->dir, &c->local);
	link_child(&c->dir, &c->remote);
	link_child(&c->dir, &c->status);
	link_child(&g_fs.root, &c->dir);
}

static struct aether_conv *conv_alloc(void)
{
	for (int i = 0; i < AETHER_MAX_CONNS; i++) {
		struct aether_conv *c = &g_fs.convs[i];

		if (!c->in_use) {
			memset(&c->peer, 0, sizeof(c->peer));
			c->in_use = true;
			c->slot = i;
			c->state = CONV_UNCONNECTED;
			k_msgq_init(&c->rxq, c->rxq_storage, sizeof(struct aether_dgram),
				    AETHER_RXQ_DEPTH);
			conv_build_nodes(c);
			return c;
		}
	}
	return NULL;
}

static void conv_free(struct aether_conv *c)
{
	if (!c->in_use) {
		return;
	}
	/* Tear down, then leave a SINGLE EOF sentinel (len 0) so a blocked reader
	 * wakes and returns EOF. Order matters: purge first to drop any queued
	 * datagrams, THEN put the sentinel last. The old order (put-then-purge)
	 * raced -- if no reader was parked at that instant the sentinel went into
	 * the ring buffer and the very next purge discarded it, so the reader (and,
	 * through the relay's proxied read, its conversation) blocked forever. */
	struct aether_dgram eof = { .len = 0 };

	unlink_child(&g_fs.root, &c->dir);
	k_msgq_purge(&c->rxq);
	(void)k_msgq_put(&c->rxq, &eof, K_NO_WAIT);
	c->in_use = false;
	c->state = CONV_UNCONNECTED;
}

/* ---- mesh receive: fan a datagram out to matching conversations ---- */

static void aether_recv_cb(struct net_if *iface, const uint8_t src[6],
			   const uint8_t *payload, size_t len, bool broadcast,
			   void *user)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(user);

	if (len > AETHER_MAX_MSG) {
		len = AETHER_MAX_MSG;
	}
	/* The app addresses peers by their durable identity (node_eui), never the
	 * churning HONR route. A broadcast frame already carries node_eui as src;
	 * a unicast frame carries the HONR src, so resolve it back to the stable
	 * identity here. If the binding isn't known yet, fall back to the raw src
	 * (announced convs still display it; a connected filter just won't match). */
	const uint8_t *ident = src;
	uint8_t ident_buf[6];

	if (!broadcast &&
	    aether_mesh_addr_to_eui(g_fs.iface, src, ident_buf) == 0) {
		ident = ident_buf;
	}
	k_mutex_lock(&g_fs.lock, K_FOREVER);
	g_fs.ctr.rx++;
	g_fs.ctr.rx_bytes += (uint32_t)len;
	for (int i = 0; i < AETHER_MAX_CONNS; i++) {
		struct aether_conv *c = &g_fs.convs[i];

		if (!c->in_use) {
			continue;
		}
		/* §6a: broadcasts fan into every party-line conv; unicast goes to
		 * announced (any peer) or a connected conv bound to this peer. */
		bool take;

		if (broadcast) {
			take = (c->state == CONV_BCAST);
		} else if (c->state == CONV_ANNOUNCED) {
			take = true;   /* accepts any unicast source */
		} else if (c->state == CONV_CONNECTED) {
			/* Match by resolving the CONNECTED peer's durable id to its
			 * current HONR route and comparing to the raw src -- the same
			 * eui->addr lookup the TX path uses, so RX matching is exactly as
			 * reliable as our ability to SEND to this peer (never fails when TX
			 * works). The direct-compare fallback covers a legacy by-HONR
			 * connect (c->peer already a route) and the resolved-eui case. */
			uint8_t route[6];

			take = (aether_mesh_eui_to_addr(g_fs.iface, c->peer, route) == 0 &&
				memcmp(route, src, 6) == 0) ||
			       memcmp(c->peer, ident, 6) == 0 ||
			       memcmp(c->peer, src, 6) == 0;
		} else {
			take = false;
		}
		if (!take) {
			continue;
		}
		struct aether_dgram d;

		memcpy(d.src, ident, 6);
		d.len = (uint16_t)len;
		memcpy(d.data, payload, len);
		if (k_msgq_put(&c->rxq, &d, K_NO_WAIT) != 0) {
			g_fs.ctr.rx_drop++;
			LOG_WRN("conv %d rxq full, dropping datagram", i);
		}
	}
	k_mutex_unlock(&g_fs.lock);
}

/* ---- ctl command grammar (spec §5) ---- */

static int ctl_exec(struct aether_conv *c, const char *cmd)
{
	if (strncmp(cmd, "connect ", 8) == 0) {
		uint8_t a[6];

		if (c->state != CONV_UNCONNECTED) {
			return -EINVAL;
		}
		if (parse_addr(cmd + 8, a) < 0) {
			return -EINVAL;
		}
		/* §6a: connect to the all-ones address marks a best-effort broadcast
		 * party line, not a unicast bind. */
		static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

		if (memcmp(a, bcast, 6) == 0) {
			c->state = CONV_BCAST;
			return 0;
		}
		memcpy(c->peer, a, 6);
		c->state = CONV_CONNECTED;
		return 0;
	}
	if (strcmp(cmd, "announce") == 0) {
		if (c->state != CONV_UNCONNECTED) {
			return -EINVAL;
		}
		c->state = CONV_ANNOUNCED;
		return 0;
	}
	if (strcmp(cmd, "hangup") == 0) {
		struct aether_dgram eof = { .len = 0 };

		c->state = CONV_UNCONNECTED;
		memset(c->peer, 0, sizeof(c->peer));
		/* Wake any blocked data reader with an EOF sentinel: purge stale
		 * datagrams first, then leave the len-0 sentinel as the last msg. */
		k_msgq_purge(&c->rxq);
		(void)k_msgq_put(&c->rxq, &eof, K_NO_WAIT);
		return 0;
	}
	return -EINVAL;
}

/* ---- fs_ops ---- */

static struct ninep_fs_node *anet_get_root(void *ctx) { ARG_UNUSED(ctx); return &g_fs.root; }

static struct ninep_fs_node *anet_walk(struct ninep_fs_node *parent, const char *name,
				       uint16_t name_len, void *ctx)
{
	ARG_UNUSED(ctx);
	if (!parent || parent->type != NINEP_NODE_DIR) {
		return NULL;
	}

	/* clone: walking it allocates a fresh conversation and yields its ctl
	 * node (so the walked fid reads "N" and takes ctl writes). */
	if (AN(parent)->kind == K_ROOT &&
	    name_len == 5 && strncmp(name, "clone", 5) == 0) {
		k_mutex_lock(&g_fs.lock, K_FOREVER);
		struct aether_conv *c = conv_alloc();
		k_mutex_unlock(&g_fs.lock);
		if (!c) {
			return NULL;   /* -ENOSPC: no free slot */
		}
		LOG_INF("/net/aether: conversation %d allocated (clone)", c->slot);
		return &c->ctl;
	}

	for (struct ninep_fs_node *ch = parent->children; ch; ch = ch->next_sibling) {
		if (strlen(ch->name) == name_len && strncmp(ch->name, name, name_len) == 0) {
			return ch;
		}
	}
	return NULL;
}

static int anet_open(struct ninep_fs_node *node, uint8_t mode, void *ctx)
{
	ARG_UNUSED(node); ARG_UNUSED(mode); ARG_UNUSED(ctx);
	return 0;
}

/* Emit a 9P directory entry (Rstat record) for @child into buf; mirrors ramfs. */
static int put_dirent(uint8_t *buf, uint32_t cap, struct ninep_fs_node *child)
{
	size_t off = 0;
	int ret = ninep_write_stat(buf, cap, &off, &child->qid, child->mode,
				   child->length, child->name, strlen(child->name),
				   NULL, NULL, NULL);
	if (ret < 0) {
		return ret;
	}
	return (int)off;
}

static int read_dir(struct ninep_fs_node *dir, uint64_t offset, uint8_t *buf, uint32_t count)
{
	size_t pos = 0;      /* byte position in the directory stream */
	size_t out = 0;

	for (struct ninep_fs_node *ch = dir->children; ch; ch = ch->next_sibling) {
		uint8_t tmp[128];
		int n = put_dirent(tmp, sizeof(tmp), ch);

		if (n < 0) {
			return n;
		}
		if (pos >= offset) {
			if (out + n > count) {
				break;
			}
			memcpy(buf + out, tmp, n);
			out += n;
		}
		pos += n;
	}
	return (int)out;
}

static int anet_read(struct ninep_fs_node *node, uint64_t offset, uint8_t *buf,
		     uint32_t count, const char *uname, void *ctx)
{
	ARG_UNUSED(uname); ARG_UNUSED(ctx);
	struct anode *an = AN(node);
	struct aether_conv *c = an->conv;
	char s[256];
	int n = 0;

	switch (an->kind) {
	case K_ROOT:
	case K_CONVDIR:
		return read_dir(node, offset, buf, count);
	case K_ADDR:
		n = fmt_addr(s, sizeof(s), my_identity());
		s[n++] = '\n';
		break;
	case K_TOPSTATUS: {
		int active = 0;

		for (int i = 0; i < AETHER_MAX_CONNS; i++) {
			active += g_fs.convs[i].in_use ? 1 : 0;
		}
		n = snprintf(s, sizeof(s), "%d/%d\n", active, AETHER_MAX_CONNS);
		break;
	}
	case K_STATS: {
		/* One-line summary: datagram rx/tx counts (whole datagrams, post-
		 * reassembly/dedup) plus the live link signal -- the RSSI/SNR of the
		 * most recent frame this node heard, straight off the PHY. */
		int rssi = 0, snr = 0;
		struct heymac_context *hc =
			g_mesh_ctx ? net_if_l2_data(g_mesh_ctx->iface) : NULL;

		if (hc) {
			rssi = hc->last_rx_rssi;
			snr = hc->last_rx_snr;
		}
		n = snprintf(s, sizeof(s), "rx %u tx %u rssi %d snr %d\n",
			     g_fs.ctr.rx, g_fs.ctr.tx, rssi, snr);
		break;
	}
	case K_MAXMSG:
		/* spec §6: advertise the reassembled-datagram ceiling so a mounter can
		 * negotiate msize <= it. One DECT frame today (no fragmentation). */
		n = snprintf(s, sizeof(s), "%d\n", AETHER_MAX_MSG);
		break;
	case K_CLONE:
	case K_CTL:
		n = snprintf(s, sizeof(s), "%d\n", c ? c->slot : 0);
		break;
	case K_LOCAL:
		n = fmt_addr(s, sizeof(s), my_identity());
		s[n++] = '\n';
		break;
	case K_REMOTE:
		if (c && c->state == CONV_CONNECTED) {
			n = fmt_addr(s, sizeof(s), c->peer);
			s[n++] = '\n';
		} else {
			n = 0;   /* unbound -> empty */
		}
		break;
	case K_CSTATUS:
		if (c && c->state == CONV_CONNECTED) {
			char a[32];

			fmt_addr(a, sizeof(a), c->peer);
			n = snprintf(s, sizeof(s), "connected aether!%s reliable\n", a);
		} else if (c && c->state == CONV_ANNOUNCED) {
			n = snprintf(s, sizeof(s), "announced\n");
		} else if (c && c->state == CONV_BCAST) {
			n = snprintf(s, sizeof(s), "broadcast best-effort\n");  /* §6a */
		} else {
			n = snprintf(s, sizeof(s), "unconnected\n");
		}
		break;
	case K_DATA: {
		/* one datagram per read; BLOCKS until one arrives or hangup (EOF) */
		struct aether_dgram d;

		if (!c) {
			return -EINVAL;
		}
		if (k_msgq_get(&c->rxq, &d, K_FOREVER) != 0) {
			return 0;
		}
		if (d.len == 0) {
			return 0;   /* hangup sentinel -> EOF */
		}
		/* §6a broadcast reads are source-prefixed, same shape as announced --
		 * confirmed against the deck's actual lobby-reader contract (see
		 * doc/dect-guidance.md): achat expects EVERY broadcast `data` read to
		 * be exactly `[6-byte src][payload]` in one atomic read, no exceptions.
		 * The 0.7.24 experiment that dropped the prefix for CONV_BCAST (on a
		 * hypothesis that the prefix was the rendering problem) had it
		 * backwards -- the prefix's ABSENCE is what breaks the deck: reads
		 * under 6 bytes get silently dropped, longer ones get the first 6
		 * payload bytes misread as a bogus source address. */
		bool src_prefixed = (c->state == CONV_ANNOUNCED || c->state == CONV_BCAST);
		uint32_t need = d.len + (src_prefixed ? 6 : 0);

		if (count < need) {
			return -EMSGSIZE;
		}
		uint32_t k = 0;

		if (src_prefixed) {
			memcpy(buf, d.src, 6);
			k = 6;
		}
		memcpy(buf + k, d.data, d.len);
		return (int)(k + d.len);
	}
	default:
		return -EINVAL;
	}

	/* simple offset-addressable string files */
	if (offset >= (uint64_t)n) {
		return 0;
	}
	uint32_t avail = n - (uint32_t)offset;
	uint32_t k = MIN(count, avail);

	memcpy(buf, s + offset, k);
	return (int)k;
}

static int anet_write(struct ninep_fs_node *node, uint64_t offset, const uint8_t *buf,
		      uint32_t count, const char *uname, void *ctx)
{
	ARG_UNUSED(offset); ARG_UNUSED(uname); ARG_UNUSED(ctx);
	struct anode *an = AN(node);
	struct aether_conv *c = an->conv;

	if (an->kind == K_CTL || an->kind == K_CLONE) {
		char cmd[64];
		uint32_t k = MIN(count, sizeof(cmd) - 1);

		memcpy(cmd, buf, k);
		cmd[k] = '\0';
		/* strip a trailing newline/space */
		while (k && (cmd[k - 1] == '\n' || cmd[k - 1] == '\r' || cmd[k - 1] == ' ')) {
			cmd[--k] = '\0';
		}
		k_mutex_lock(&g_fs.lock, K_FOREVER);
		int ret = c ? ctl_exec(c, cmd) : -EINVAL;
		k_mutex_unlock(&g_fs.lock);
		return ret < 0 ? ret : (int)count;
	}

	if (an->kind == K_DATA) {
		const uint8_t *dst, *payload;
		uint32_t plen;
		uint8_t route[6];   /* durable peer id -> current HONR route */

		if (!c) {
			return -ENOTCONN;
		}
		if (c->state == CONV_BCAST) {
			/* §6a: best-effort broadcast -- no ARQ, no fragmentation, no dst
			 * prefix; one PHY frame max (a larger write -> -EMSGSIZE). The
			 * mesh layer floods it (TTL+dedup) for multi-hop reach. */
			static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

			if (count > CONFIG_AETHER_MAX_PAYLOAD) {
				return -EMSGSIZE;
			}
			/* Absorb a transient busy channel (LBT) before surfacing it: the
			 * client (achat lobby) shouldn't see -EBUSY on a momentary busy. */
			int ret = -EBUSY;
			for (int b = 0; b < NET_BCAST_BUSY_RETRIES && ret == -EBUSY; b++) {
				if (b > 0) {
					k_msleep(NET_BCAST_BUSY_BACKOFF_MS +
						 (sys_rand32_get() % NET_BCAST_BUSY_BACKOFF_MS));
				}
				ret = aether_mesh_send(g_fs.iface, bcast, buf, count,
						       AETHER_PRIORITY_NORMAL);
			}
			if (ret < 0) {
				g_fs.ctr.tx_err++;
				return ret;
			}
			g_fs.ctr.tx++;
			g_fs.ctr.tx_bytes += count;
			return (int)count;
		}
		if (c->state == CONV_CONNECTED) {
			/* connected: bare payload -> the bound peer (spec §4). c->peer is a
			 * durable node_eui; resolve it to the peer's CURRENT HONR route. If
			 * the binding isn't known (e.g. a non-neighbor addressed directly by
			 * its HONR addr for multi-hop), fall through to the address as given
			 * -- node_euis and HONR addrs don't collide, so this preserves the
			 * legacy by-HONR path while making by-eui churn-robust. */
			if (aether_mesh_eui_to_addr(g_fs.iface, c->peer, route) == 0) {
				dst = route;
			} else {
				dst = c->peer;
			}
			payload = buf;
			plen = count;
		} else if (c->state == CONV_ANNOUNCED) {
			/* announced: [dst][payload] -> reply to a requester (spec §4/§8),
			 * symmetric with the source-prefixed announced read. This is what
			 * lets a node be a 9P server/exporter over the mesh. The [dst] prefix
			 * is a durable node_eui (matching the src an announced read reports);
			 * resolve it to the current route, same passthrough fallback. */
			if (count < 6) {
				return -EINVAL;
			}
			if (aether_mesh_eui_to_addr(g_fs.iface, buf, route) == 0) {
				dst = route;
			} else {
				dst = buf;
			}
			payload = buf + 6;
			plen = count - 6;
		} else {
			return -ENOTCONN;
		}
		if (plen > AETHER_MAX_MSG) {
			return -EMSGSIZE;
		}
		int ret = aether_mesh_send_reliable(g_fs.iface, dst, payload, plen,
						    AETHER_NET_RETRIES);
		if (ret < 0) {
			g_fs.ctr.tx_err++;
			return ret;
		}
		g_fs.ctr.tx++;
		g_fs.ctr.tx_bytes += plen;
		return (int)count;   /* whole write consumed (incl. any dst prefix) */
	}
	return -EROFS;
}

static int anet_stat(struct ninep_fs_node *node, uint8_t *buf, size_t buf_len, void *ctx)
{
	ARG_UNUSED(ctx);
	size_t off = 0;
	int ret = ninep_write_stat(buf, buf_len, &off, &node->qid, node->mode,
				   node->length, node->name, strlen(node->name),
				   NULL, NULL, NULL);
	return ret < 0 ? ret : (int)off;
}

static int anet_clunk(struct ninep_fs_node *node, void *ctx)
{
	ARG_UNUSED(ctx);
	struct anode *an = AN(node);

	/*
	 * Tear the conversation down when its ctl fid is clunked -- ctl is the
	 * fid you get from `clone` and hold for the conversation's life (spec
	 * §5), so clunking it is the canonical hang-up. Do NOT tear down on a
	 * convdir clunk: a multi-element walk (e.g. clone then walk N/data) and
	 * an `ls N` both clunk the convdir fid incidentally, which must not free
	 * a conversation the caller is still using.
	 */
	if (an->conv && an->kind == K_CTL) {
		k_mutex_lock(&g_fs.lock, K_FOREVER);
		conv_free(an->conv);
		k_mutex_unlock(&g_fs.lock);
	}
	return 0;
}

/* Only the per-conversation data file blocks (it waits for a datagram); every
 * other node returns promptly. Lets the server dispatch data reads to a worker
 * so one blocked reader doesn't freeze the whole 9P server. */
static int anet_read_will_block(struct ninep_fs_node *node, void *ctx)
{
	ARG_UNUSED(ctx);
	return AN(node)->kind == K_DATA ? 1 : 0;
}

static const struct ninep_fs_ops aether_net_ops = {
	.get_root = anet_get_root,
	.walk = anet_walk,
	.open = anet_open,
	.read = anet_read,
	.write = anet_write,
	.stat = anet_stat,
	.clunk = anet_clunk,
	.read_will_block = anet_read_will_block,
};

const struct ninep_fs_ops *aether_net_get_ops(void) { return &aether_net_ops; }
void *aether_net_get_ctx(void) { return &g_fs; }

int aether_net_init(struct net_if *iface, const uint8_t myaddr[6])
{
	memset(&g_fs, 0, sizeof(g_fs));
	k_mutex_init(&g_fs.lock);
	g_fs.iface = iface;
	g_fs.next_qid = 1;
	memcpy(g_fs.myaddr, myaddr, 6);

	node_init(&g_fs.root, "/", NINEP_NODE_DIR, &g_fs.an_root, K_ROOT, NULL);
	node_init(&g_fs.clone, "clone", NINEP_NODE_FILE, &g_fs.an_clone, K_CLONE, NULL);
	node_init(&g_fs.topstatus, "status", NINEP_NODE_FILE, &g_fs.an_topstatus, K_TOPSTATUS, NULL);
	node_init(&g_fs.addr, "addr", NINEP_NODE_FILE, &g_fs.an_addr, K_ADDR, NULL);
	node_init(&g_fs.statf, "stats", NINEP_NODE_FILE, &g_fs.an_statf, K_STATS, NULL);
	node_init(&g_fs.maxmsgf, "maxmsg", NINEP_NODE_FILE, &g_fs.an_maxmsgf, K_MAXMSG, NULL);
	link_child(&g_fs.root, &g_fs.clone);
	link_child(&g_fs.root, &g_fs.topstatus);
	link_child(&g_fs.root, &g_fs.addr);
	link_child(&g_fs.root, &g_fs.statf);
	link_child(&g_fs.root, &g_fs.maxmsgf);

	int ret = aether_mesh_register_recv_callback(iface, aether_recv_cb, NULL);

	LOG_INF("/net/aether datagram service up (%d conversations); recv_cb=%d",
		AETHER_MAX_CONNS, ret);
	return ret;
}
