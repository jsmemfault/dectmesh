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
#include "cga.h"   /* cga_get_pubkey / cga_sign for net/aether/prove */

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
#define AETHER_NET_RETRIES 5     /* reliable-send retry budget; with the ACK-timeout
				  * backoff (aether_route.c) this rides out two-way
				  * contention instead of failing at 3 lock-step retries */

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
	K_PROVE,
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

	struct ninep_fs_node root, clone, topstatus, addr, statf, maxmsgf, provef;
	struct anode an_root, an_clone, an_topstatus, an_addr, an_statf, an_maxmsgf, an_provef;

	/* Stage-2 CGA ownership proof: hex-encoded "pubkey signature" generated when
	 * a challenge is written to net/aether/prove (signed at write, served at
	 * read). Empty until the first challenge. */
	uint8_t prove_out[264];
	uint16_t prove_out_len;

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

/* Drain diagnostics, surfaced in the dev/mflt_mesh output (the 9151 console is
 * unreliable under a concurrent 9P session -- shared DTR). Set by the drain path,
 * read by gen_mflt_mesh. */
int g_drain_sr = -99;   /* last send_reliable() return */
int g_drain_gr = -99;   /* last k_msgq_get() return (0 = got a datagram) */
int g_drain_step = 0;   /* furthest step: 1=ver 2=att 3=walk 4=open 5=read */

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
	if (!broadcast && len >= 7) {
		LOG_INF("recv unicast src=%02x:%02x:%02x:%02x:%02x:%02x R%u len=%d",
			src[0], src[1], src[2], src[3], src[4], src[5],
			payload[4], (int)len);
	}
	k_mutex_lock(&g_fs.lock, K_FOREVER);
	g_fs.ctr.rx++;
	g_fs.ctr.rx_bytes += (uint32_t)len;
	int took = 0;
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
		} else {
			took++;
		}
	}
	k_mutex_unlock(&g_fs.lock);
	if (!broadcast && len >= 7 && took == 0) {
		LOG_INF("recv unicast: NO conv took it (no CONNECTED match)");
	}
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
	case K_PROVE: {
		/* Serve the "pubkey signature" hex proof generated when the challenge
		 * was written (offset-addressable). Empty until a nonce is written. */
		if (offset >= (uint64_t)g_fs.prove_out_len) {
			return 0;
		}
		uint32_t avail = g_fs.prove_out_len - (uint32_t)offset;
		uint32_t k = MIN(count, avail);

		memcpy(buf, g_fs.prove_out + offset, k);
		return (int)k;
	}
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

	if (an->kind == K_PROVE) {
		/* Stage-2 ownership proof: the written bytes are the verifier's
		 * challenge. Sign them with this node's CGA private key NOW and cache
		 * "pubkey_hex signature_hex" for the following read. A fresh signature
		 * over a caller-chosen nonce proves live key possession (not a replay);
		 * pairing it with pubkey lets the verifier also check
		 * node_eui == SHA256(pubkey)[:6]. */
		static const char hexd[] = "0123456789abcdef";
		uint8_t nonce[32], pub[64], sig[64];
		uint32_t nlen = MIN(count, sizeof(nonce));

		memcpy(nonce, buf, nlen);
		if (cga_get_pubkey(pub) < 0 || cga_sign(nonce, nlen, sig) < 0) {
			k_mutex_lock(&g_fs.lock, K_FOREVER);
			g_fs.prove_out_len = 0;
			k_mutex_unlock(&g_fs.lock);
			return -EIO;
		}
		k_mutex_lock(&g_fs.lock, K_FOREVER);
		int p = 0;

		for (int i = 0; i < 64; i++) {
			g_fs.prove_out[p++] = hexd[pub[i] >> 4];
			g_fs.prove_out[p++] = hexd[pub[i] & 0x0f];
		}
		g_fs.prove_out[p++] = ' ';
		for (int i = 0; i < 64; i++) {
			g_fs.prove_out[p++] = hexd[sig[i] >> 4];
			g_fs.prove_out[p++] = hexd[sig[i] & 0x0f];
		}
		g_fs.prove_out[p++] = '\n';
		g_fs.prove_out_len = (uint16_t)p;
		k_mutex_unlock(&g_fs.lock);
		return (int)count;
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

/* ---- on-device mesh 9P client: drain a peer's dev/mflt over the air ---------
 * Runs the whole 9P session (version/attach/walk/open/read) to <peer_eui>'s mesh
 * 9P server IN-PROCESS over a conversation's reliable datagrams -- so NONE of it
 * crosses the inter-chip uart1. Only the drained result crosses (once, when the
 * relay reads dev/mflt_mesh). This replaces the host-driven bridge, whose every
 * peer 9P op was a uart1 round-trip (dozens/cycle) that saturated the control
 * plane and wedged the relay's reads. Runs in the 9P-server thread context that
 * services the dev/mflt_mesh read; blocks on the conversation rxq, which the RX
 * thread fills -- no uart1 in the loop. */
static void np16(uint8_t *b, uint16_t v) { b[0] = v; b[1] = v >> 8; }
static void np32(uint8_t *b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static uint16_t ng16(const uint8_t *b) { return b[0] | (b[1] << 8); }
static uint32_t ng32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }

/* Send one framed 9P T (msg[0..len), size stamped here) to the connected peer and
 * return the tag-matched R datagram in rbuf (kept framed: type@4, tag@5, ...).
 * Retries a lost T; discards stale/duplicate R's (the datagram channel is
 * untagged at the mesh layer). Returns reply length, or <0 on failure. */
static int m9_rpc(struct aether_conv *c, uint8_t *msg, int len, uint8_t *rbuf, int cap)
{
	np32(msg, (uint32_t)len);
	uint16_t want = ng16(msg + 5);
	/* c->peer is the durable node_eui; resolve it to the peer's current HONR route
	 * exactly as anet_write's CONNECTED send does (fallback to the eui). */
	uint8_t route[6];
	const uint8_t *dst = c->peer;

	if (aether_mesh_eui_to_addr(g_fs.iface, c->peer, route) == 0) {
		dst = route;
	}
	for (int attempt = 0; attempt < 2; attempt++) {
		int sr = aether_mesh_send_reliable(g_fs.iface, dst, msg, len,
						   AETHER_NET_RETRIES);
		g_drain_sr = sr;
		if (sr < 0) {
			continue;
		}
		for (int reads = 0; reads < 2; reads++) {
			struct aether_dgram d;
			int gr = k_msgq_get(&c->rxq, &d, K_MSEC(1500));

			g_drain_gr = gr;
			if (gr != 0) {
				break;              /* no reply this attempt -> resend */
			}
			if (d.len == 0) {
				return -1;          /* hangup/EOF */
			}
			if (d.len >= 7 && ng16(d.data + 5) == want) {
				int n = MIN((int)d.len, cap);

				memcpy(rbuf, d.data, n);
				return n;
			}
			/* stale tag: keep reading within this attempt */
		}
	}
	return -1;
}

int aether_net_drain_peer(const uint8_t peer_eui[6], uint8_t *out, size_t cap)
{
	k_mutex_lock(&g_fs.lock, K_FOREVER);
	struct aether_conv *c = conv_alloc();

	k_mutex_unlock(&g_fs.lock);
	if (!c) {
		LOG_WRN("drain %02x:%02x: conv_alloc FAILED (pool full)", peer_eui[4], peer_eui[5]);
		return -ENOSPC;
	}
	memcpy(c->peer, peer_eui, 6);
	c->state = CONV_CONNECTED;

	uint8_t msg[64];
	uint8_t rbuf[AETHER_MAX_MSG];
	uint16_t tag = 1;
	int total = 0, o, r;

	/* Tversion */
	o = 4; msg[o++] = 100; np16(msg + o, 0xffff); o += 2;
	np32(msg + o, AETHER_MAX_MSG); o += 4;
	np16(msg + o, 6); o += 2; memcpy(msg + o, "9P2000", 6); o += 6;
	r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
	LOG_INF("drain %02x:%02x: version r=%d type=%d", peer_eui[4], peer_eui[5],
		r, r > 4 ? rbuf[4] : -1);
	if (r < 5 || rbuf[4] != 101) { goto done; }

	/* Tattach fid 0 */
	o = 4; msg[o++] = 104; np16(msg + o, tag++); o += 2;
	np32(msg + o, 0); o += 4; np32(msg + o, 0xffffffffu); o += 4;
	np16(msg + o, 1); o += 2; msg[o++] = 't'; np16(msg + o, 0); o += 2;
	r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
	if (r < 5 || rbuf[4] != 105) { goto done; }

	/* Twalk fid 0 -> 2: ["dev","mflt"] (one round-trip) */
	o = 4; msg[o++] = 110; np16(msg + o, tag++); o += 2;
	np32(msg + o, 0); o += 4; np32(msg + o, 2); o += 4;
	np16(msg + o, 2); o += 2;
	np16(msg + o, 3); o += 2; memcpy(msg + o, "dev", 3); o += 3;
	np16(msg + o, 4); o += 2; memcpy(msg + o, "mflt", 4); o += 4;
	r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
	if (r < 5 || rbuf[4] != 111) { goto done; }   /* Rwalk=111 */

	/* Topen fid 2 OREAD */
	o = 4; msg[o++] = 112; np16(msg + o, tag++); o += 2;
	np32(msg + o, 2); o += 4; msg[o++] = 0;
	r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
	if (r < 5 || rbuf[4] != 113) { goto done; }   /* Ropen=113 */

	/* Tread loop: drain successive chunk batches (the gen is stateful, so a
	 * repeated offset-0 read yields the next batch until empty). */
	for (int i = 0; i < 16 && total < (int)cap - 8; i++) {
		uint32_t want_n = (uint32_t)(AETHER_MAX_MSG - 16);

		o = 4; msg[o++] = 116; np16(msg + o, tag++); o += 2;
		np32(msg + o, 2); o += 4;
		np32(msg + o, 0); o += 4; np32(msg + o, 0); o += 4;
		np32(msg + o, want_n); o += 4;
		r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
		if (r < 11 || rbuf[4] != 117) { break; }  /* Rread=117 */
		uint32_t n = ng32(rbuf + 7);

		if ((int)n > r - 11) { n = r - 11; }
		if ((int)n > (int)cap - 1 - total) { n = cap - 1 - total; }
		if (n == 0) { break; }
		memcpy(out + total, rbuf + 11, n);
		total += n;
	}
done:
	k_mutex_lock(&g_fs.lock, K_FOREVER);
	conv_free(c);
	k_mutex_unlock(&g_fs.lock);
	return total;
}

/*
 * Fast single-RPC probe: send ONE Tversion to the peer and wait briefly for the
 * Rversion. Bounded to a couple of seconds so a read can return the result while
 * the client is still listening (unlike a full drain, whose blocking send_reliable
 * retries against a silent peer outlast the client watchdog). Writes a one-line
 * human summary of send-result (sr), get-result (gr), and the reply type. */
int aether_net_probe_peer(const uint8_t peer_eui[6], char *out, size_t cap)
{
	k_mutex_lock(&g_fs.lock, K_FOREVER);
	struct aether_conv *c = conv_alloc();

	k_mutex_unlock(&g_fs.lock);
	if (!c) {
		return snprintf(out, cap, "PROBE %02x%02x%02x%02x%02x%02x: conv_alloc FAILED\n",
			peer_eui[0], peer_eui[1], peer_eui[2],
			peer_eui[3], peer_eui[4], peer_eui[5]);
	}
	memcpy(c->peer, peer_eui, 6);
	c->state = CONV_CONNECTED;

	/* Resolve eui -> current HONR route (same as anet_write's CONNECTED send). */
	uint8_t route[6];
	const uint8_t *dst = c->peer;
	int resolved = aether_mesh_eui_to_addr(g_fs.iface, c->peer, route);

	if (resolved == 0) {
		dst = route;
	}

	/* Run the full 9P session (version->attach->walk->open->read) exactly as
	 * drain_peer does, but record the reply type at each step so a read that
	 * returns empty tells us precisely which step failed. m9_rpc stamps the size
	 * prefix and resolves the route itself. */
	uint8_t msg[64];
	uint8_t rbuf[AETHER_MAX_MSG];
	uint16_t tag = 1;
	int o, r;
	int t_ver = -1, t_att = -1, t_walk = -1, t_open = -1, rdbytes = -1;

	/* Tversion */
	o = 4; msg[o++] = 100; np16(msg + o, 0xffff); o += 2;
	np32(msg + o, AETHER_MAX_MSG); o += 4;
	np16(msg + o, 6); o += 2; memcpy(msg + o, "9P2000", 6); o += 6;
	r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
	t_ver = (r > 4) ? rbuf[4] : r;
	if (r >= 5 && rbuf[4] == 101) {
		/* Tattach fid 0 */
		o = 4; msg[o++] = 104; np16(msg + o, tag++); o += 2;
		np32(msg + o, 0); o += 4; np32(msg + o, 0xffffffffu); o += 4;
		np16(msg + o, 1); o += 2; msg[o++] = 't'; np16(msg + o, 0); o += 2;
		r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
		t_att = (r > 4) ? rbuf[4] : r;
	}
	if (t_att == 105) {
		/* Twalk fid 0 -> 2: ["dev","mflt"] */
		o = 4; msg[o++] = 110; np16(msg + o, tag++); o += 2;
		np32(msg + o, 0); o += 4; np32(msg + o, 2); o += 4;
		np16(msg + o, 2); o += 2;
		np16(msg + o, 3); o += 2; memcpy(msg + o, "dev", 3); o += 3;
		np16(msg + o, 4); o += 2; memcpy(msg + o, "mflt", 4); o += 4;
		r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
		t_walk = (r > 4) ? rbuf[4] : r;
	}
	if (t_walk == 111) {
		/* Topen fid 2 OREAD */
		o = 4; msg[o++] = 112; np16(msg + o, tag++); o += 2;
		np32(msg + o, 2); o += 4; msg[o++] = 0;
		r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
		t_open = (r > 4) ? rbuf[4] : r;
	}
	if (t_open == 113) {
		/* One Tread */
		o = 4; msg[o++] = 116; np16(msg + o, tag++); o += 2;
		np32(msg + o, 2); o += 4;
		np32(msg + o, 0); o += 4; np32(msg + o, 0); o += 4;
		np32(msg + o, (uint32_t)(AETHER_MAX_MSG - 16)); o += 4;
		r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
		if (r >= 11 && rbuf[4] == 117) {
			rdbytes = (int)ng32(rbuf + 7);
		} else {
			rdbytes = (r > 4) ? -(int)rbuf[4] : r;   /* -type or errno */
		}
	}

	k_mutex_lock(&g_fs.lock, K_FOREVER);
	conv_free(c);
	k_mutex_unlock(&g_fs.lock);

	return snprintf(out, cap,
		"PROBE %02x%02x%02x%02x%02x%02x route=%02x%02x%02x%02x%02x%02x(r=%d) "
		"ver=%d att=%d walk=%d open=%d read=%d\n",
		peer_eui[0], peer_eui[1], peer_eui[2], peer_eui[3], peer_eui[4], peer_eui[5],
		dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], resolved,
		t_ver, t_att, t_walk, t_open, rdbytes);
}

/* ---- OTA over the mesh: push a signed image (or a control write) to a peer ----
 *
 * The write twin of aether_net_drain_peer. A peer's mesh 9P server already serves
 * its own dev/firmware (ninep_dfu -> MCUboot secondary slot), dev/reboot and
 * dev/confirm over the mesh (same namespace as USB/UART), so OTA-over-mesh is a
 * pure CLIENT feature here: run a 9P write session to the peer over a conversation.
 *
 * The image (~300 KB) will NOT fit in the 9151's RAM, so the push STREAMS: the host
 * feeds chunks to dev/push and each is forwarded as a mesh Twrite to the peer's
 * held dev/firmware fid. The session (conv + fid + offset) lives in g_push across
 * many sysfs write calls -- exactly the relay's fw9151_write pattern, mesh-side. */
static struct {
	struct aether_conv *conv;
	uint8_t  peer[6];
	uint32_t fid;       /* peer's open dev/firmware fid */
	uint16_t tag;
	uint64_t woff;      /* bytes streamed so far */
	bool     active;
} g_push;

/* Build a 9P session on conv c up to an open fid 2 for dev/<file> with `mode`.
 * version -> attach(fid 0) -> walk(0->2 ["dev", file]) -> open(fid 2, mode).
 * Returns 0 on success (fid 2 open), or a negative step code for diagnostics. */
static int m9_open_devfile(struct aether_conv *c, uint16_t *tag,
			   const char *file, uint8_t mode)
{
	uint8_t msg[80];
	uint8_t rbuf[AETHER_MAX_MSG];
	int o, r;
	int flen = (int)strlen(file);

	/* Tversion */
	o = 4; msg[o++] = 100; np16(msg + o, 0xffff); o += 2;
	np32(msg + o, AETHER_MAX_MSG); o += 4;
	np16(msg + o, 6); o += 2; memcpy(msg + o, "9P2000", 6); o += 6;
	r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
	if (r < 5 || rbuf[4] != 101) { return -1; }

	/* Tattach fid 0 */
	o = 4; msg[o++] = 104; np16(msg + o, (*tag)++); o += 2;
	np32(msg + o, 0); o += 4; np32(msg + o, 0xffffffffu); o += 4;
	np16(msg + o, 1); o += 2; msg[o++] = 't'; np16(msg + o, 0); o += 2;
	r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
	if (r < 5 || rbuf[4] != 105) { return -2; }

	/* Twalk fid 0 -> 2: ["dev", file] */
	o = 4; msg[o++] = 110; np16(msg + o, (*tag)++); o += 2;
	np32(msg + o, 0); o += 4; np32(msg + o, 2); o += 4;
	np16(msg + o, 2); o += 2;
	np16(msg + o, 3); o += 2; memcpy(msg + o, "dev", 3); o += 3;
	np16(msg + o, flen); o += 2; memcpy(msg + o, file, flen); o += flen;
	r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
	if (r < 5 || rbuf[4] != 111) { return -3; }   /* Rwalk */

	/* Topen fid 2 mode */
	o = 4; msg[o++] = 112; np16(msg + o, (*tag)++); o += 2;
	np32(msg + o, 2); o += 4; msg[o++] = mode;
	r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));
	if (r < 5 || rbuf[4] != 113) { return -4; }   /* Ropen */

	return 0;
}

/* Open a streaming DFU write session to peer's dev/firmware. Holds g_push. */
int aether_net_push_open(const uint8_t peer_eui[6])
{
	if (g_push.active) {
		/* A prior push was aborted without a commit -- tear it down so a fresh
		 * target never wedges on a stale session (idempotent, self-healing). */
		k_mutex_lock(&g_fs.lock, K_FOREVER);
		conv_free(g_push.conv);
		k_mutex_unlock(&g_fs.lock);
		g_push.active = false;
		g_push.conv = NULL;
	}
	k_mutex_lock(&g_fs.lock, K_FOREVER);
	struct aether_conv *c = conv_alloc();

	k_mutex_unlock(&g_fs.lock);
	if (!c) {
		return -ENOSPC;
	}
	memcpy(c->peer, peer_eui, 6);
	c->state = CONV_CONNECTED;

	g_push.tag = 1;
	int r = m9_open_devfile(c, &g_push.tag, "firmware", 1 /* OWRITE */);

	if (r < 0) {
		LOG_WRN("push %02x:%02x: open dev/firmware failed at step %d",
			peer_eui[4], peer_eui[5], r);
		k_mutex_lock(&g_fs.lock, K_FOREVER);
		conv_free(c);
		k_mutex_unlock(&g_fs.lock);
		return r;
	}
	g_push.conv = c;
	memcpy(g_push.peer, peer_eui, 6);
	g_push.fid = 2;
	g_push.woff = 0;
	g_push.active = true;
	LOG_INF("push %02x:%02x: dev/firmware open, streaming", peer_eui[4], peer_eui[5]);
	return 0;
}

/* Stream `count` bytes at `offset` to the peer's dev/firmware, splitting into
 * mesh-payload-sized Twrites. Returns bytes accepted (>=0) or negative. */
int aether_net_push_write(const uint8_t *buf, uint32_t count, uint64_t offset)
{
	if (!g_push.active) {
		return -EPIPE;
	}
	/* Twrite header = size[4]+type[1]+tag[2]+fid[4]+offset[8]+count[4] = 23.
	 * Cap the data so the whole 9P Twrite stays well within the mesh payload the
	 * reliable path actually carries (the drain proved ~356-byte datagrams; a full
	 * 448 wedged the send). Static buffers: one push session at a time, no reentry. */
	const uint32_t hdr = 23;
	uint32_t maxdata = AETHER_MAX_MSG - hdr;

	if (maxdata > 256) { maxdata = 256; }
	static uint8_t msg[AETHER_MAX_MSG];
	static uint8_t rbuf[64];
	uint32_t done = 0;

	extern int g_drain_sr;

	while (done < count) {
		uint32_t n = count - done;

		if (n > maxdata) { n = maxdata; }

		/* The peer's mesh 9P server sends its reply with a BLOCKING reliable
		 * unicast on its RX thread (aether_9p_mesh), so while it is finishing the
		 * previous Rwrite it cannot receive+ACK our next Twrite -> our send times
		 * out (sr=-ETIMEDOUT). Two mitigations: settle briefly so the peer frees
		 * up first, and -- because a send that never got ACKed was never delivered
		 * (the peer did NOT process it) -- safely retry the whole Twrite with a
		 * fresh tag. Only a delivered-but-reply-lost case would be ambiguous, and
		 * the 11-byte Rwrite is itself sent reliably, so that is rare. */
		int r = -1;
		uint32_t wr = 0;

		for (int attempt = 0; attempt < 10; attempt++) {
			if (offset + done > 0 || attempt > 0) {
				k_msleep(60 + attempt * 40);   /* settle + backoff */
			}
			int o = 4;

			msg[o++] = 118; np16(msg + o, g_push.tag++); o += 2;   /* Twrite */
			np32(msg + o, g_push.fid); o += 4;
			np32(msg + o, (uint32_t)(offset + done)); o += 4;
			np32(msg + o, (uint32_t)((offset + done) >> 32)); o += 4;
			np32(msg + o, n); o += 4;
			memcpy(msg + o, buf + done, n); o += n;

			r = m9_rpc(g_push.conv, msg, o, rbuf, sizeof(rbuf));
			if (r >= 11 && rbuf[4] == 119) {   /* Rwrite */
				wr = ng32(rbuf + 7);
				break;
			}
			if (g_drain_sr >= 0) {
				/* delivered but no/!Rwrite reply -- ambiguous, don't re-deliver. */
				break;
			}
			/* sr<0: not delivered -> safe to retry. */
		}
		if (r < 11 || rbuf[4] != 119) {
			LOG_ERR("push: Twrite failed at off %llu (r=%d sr=%d)",
				offset + done, r, g_drain_sr);
			return -EIO;
		}
		if (wr == 0) { break; }
		done += wr;
		if (wr < n) { break; }   /* short write */
	}
	g_push.woff = offset + done;
	return (int)done;
}

/* Finalize: clunk the peer's dev/firmware fid -> its ninep_dfu requests the
 * upgrade. Frees the conversation. */
int aether_net_push_close(void)
{
	if (!g_push.active) {
		return -EPIPE;
	}
	uint8_t msg[16];
	uint8_t rbuf[AETHER_MAX_MSG];
	int o = 4;

	msg[o++] = 120; np16(msg + o, g_push.tag++); o += 2;   /* Tclunk */
	np32(msg + o, g_push.fid); o += 4;
	(void)m9_rpc(g_push.conv, msg, o, rbuf, sizeof(rbuf));   /* Rclunk best-effort */

	k_mutex_lock(&g_fs.lock, K_FOREVER);
	conv_free(g_push.conv);
	k_mutex_unlock(&g_fs.lock);
	LOG_INF("push %02x:%02x: DFU stream finalized (%llu bytes); peer will upgrade",
		g_push.peer[4], g_push.peer[5], g_push.woff);
	g_push.active = false;
	g_push.conv = NULL;
	return 0;
}

/* One-shot control write to a peer's dev/<file> ("1"), over the mesh. Used for
 * dev/reboot (swap) and dev/confirm (mark good). If expect_reset, the peer resets
 * mid-reply so a missing Rwrite/Rclunk is success once the request is on the wire. */
int aether_net_push_ctl(const uint8_t peer_eui[6], const char *file, bool expect_reset)
{
	k_mutex_lock(&g_fs.lock, K_FOREVER);
	struct aether_conv *c = conv_alloc();

	k_mutex_unlock(&g_fs.lock);
	if (!c) {
		return -ENOSPC;
	}
	memcpy(c->peer, peer_eui, 6);
	c->state = CONV_CONNECTED;

	uint16_t tag = 1;
	int r = m9_open_devfile(c, &tag, file, 1 /* OWRITE */);

	if (r < 0) {
		k_mutex_lock(&g_fs.lock, K_FOREVER);
		conv_free(c);
		k_mutex_unlock(&g_fs.lock);
		LOG_WRN("push_ctl %02x:%02x dev/%s: open failed step %d",
			peer_eui[4], peer_eui[5], file, r);
		return r;
	}

	uint8_t msg[32];
	uint8_t rbuf[AETHER_MAX_MSG];
	int o = 4;

	msg[o++] = 118; np16(msg + o, tag++); o += 2;   /* Twrite fid 2 "1" */
	np32(msg + o, 2); o += 4;
	np32(msg + o, 0); o += 4; np32(msg + o, 0); o += 4;
	np32(msg + o, 1); o += 4; msg[o++] = '1';
	r = m9_rpc(c, msg, o, rbuf, sizeof(rbuf));

	int ret = 0;

	if (expect_reset) {
		ret = 0;   /* peer reboots mid-reply; the write is on the wire = success */
	} else if (r < 11 || rbuf[4] != 119) {
		ret = -EIO;
	}

	/* Tclunk (best-effort; skip fuss if the peer already reset). */
	o = 4; msg[o++] = 120; np16(msg + o, tag++); o += 2; np32(msg + o, 2); o += 4;
	(void)m9_rpc(c, msg, o, rbuf, sizeof(rbuf));

	k_mutex_lock(&g_fs.lock, K_FOREVER);
	conv_free(c);
	k_mutex_unlock(&g_fs.lock);
	LOG_INF("push_ctl %02x:%02x dev/%s: %s", peer_eui[4], peer_eui[5], file,
		ret == 0 ? "ok" : "FAILED");
	return ret;
}

void aether_net_get_stats(uint32_t *data_tx, uint32_t *data_rx, uint32_t *rxq_drops)
{
	if (data_tx) {
		*data_tx = g_fs.ctr.tx;
	}
	if (data_rx) {
		*data_rx = g_fs.ctr.rx;
	}
	if (rxq_drops) {
		*rxq_drops = g_fs.ctr.rx_drop;
	}
}

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
	node_init(&g_fs.provef, "prove", NINEP_NODE_FILE, &g_fs.an_provef, K_PROVE, NULL);
	link_child(&g_fs.root, &g_fs.clone);
	link_child(&g_fs.root, &g_fs.topstatus);
	link_child(&g_fs.root, &g_fs.addr);
	link_child(&g_fs.root, &g_fs.statf);
	link_child(&g_fs.root, &g_fs.maxmsgf);
	link_child(&g_fs.root, &g_fs.provef);

	int ret = aether_mesh_register_recv_callback(iface, aether_recv_cb, NULL);

	LOG_INF("/net/aether datagram service up (%d conversations); recv_cb=%d",
		AETHER_MAX_CONNS, ret);
	return ret;
}
