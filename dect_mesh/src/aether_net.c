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
 *       <N>/             one conversation (N = 0 .. MAX_CONNS-1)
 *           ctl          write: connect <addr> | announce | hangup ; read "N\n"
 *           data         one datagram per read/write (read blocks)
 *           local        local address
 *           remote       peer address, or empty if unbound
 *           status       connected <addr> reliable | announced | unconnected
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/protocol.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

LOG_MODULE_REGISTER(aether_net, LOG_LEVEL_INF);

#define AETHER_MAX_CONNS   4
#define AETHER_MAX_MSG     512
#define AETHER_RXQ_DEPTH   4
#define AETHER_NET_RETRIES 3     /* reliable-send retry budget (DECT PHY also helps) */

/* One received datagram queued for a conversation's data reader. */
struct aether_dgram {
	uint8_t src[6];
	uint16_t len;                      /* 0 => hangup sentinel (EOF) */
	uint8_t data[AETHER_MAX_MSG];
};

enum conv_state { CONV_UNCONNECTED = 0, CONV_CONNECTED, CONV_ANNOUNCED };

/* node "kind" so an fs_ops callback can dispatch from the bare node. Stored in
 * ninep_fs_node.data. */
enum anode_kind {
	K_ROOT = 0, K_CLONE, K_TOPSTATUS, K_ADDR,
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

	struct ninep_fs_node root, clone, topstatus, addr;
	struct anode an_root, an_clone, an_topstatus, an_addr;

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
	/* wake a blocked reader with EOF, then tear down */
	struct aether_dgram eof = { .len = 0 };

	(void)k_msgq_put(&c->rxq, &eof, K_NO_WAIT);
	unlink_child(&g_fs.root, &c->dir);
	k_msgq_purge(&c->rxq);
	c->in_use = false;
	c->state = CONV_UNCONNECTED;
}

/* ---- mesh receive: fan a datagram out to matching conversations ---- */

static void aether_recv_cb(struct net_if *iface, const uint8_t src[6],
			   const uint8_t *payload, size_t len, void *user)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(user);

	if (len > AETHER_MAX_MSG) {
		len = AETHER_MAX_MSG;
	}
	k_mutex_lock(&g_fs.lock, K_FOREVER);
	for (int i = 0; i < AETHER_MAX_CONNS; i++) {
		struct aether_conv *c = &g_fs.convs[i];

		if (!c->in_use) {
			continue;
		}
		bool take = (c->state == CONV_ANNOUNCED) ||
			    (c->state == CONV_CONNECTED && memcmp(c->peer, src, 6) == 0);
		if (!take) {
			continue;
		}
		struct aether_dgram d;

		memcpy(d.src, src, 6);
		d.len = (uint16_t)len;
		memcpy(d.data, payload, len);
		if (k_msgq_put(&c->rxq, &d, K_NO_WAIT) != 0) {
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
		c->state = CONV_UNCONNECTED;
		memset(c->peer, 0, sizeof(c->peer));
		k_msgq_purge(&c->rxq);
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
	char s[96];
	int n = 0;

	switch (an->kind) {
	case K_ROOT:
	case K_CONVDIR:
		return read_dir(node, offset, buf, count);
	case K_ADDR:
		n = fmt_addr(s, sizeof(s), g_fs.myaddr);
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
	case K_CLONE:
	case K_CTL:
		n = snprintf(s, sizeof(s), "%d\n", c ? c->slot : 0);
		break;
	case K_LOCAL:
		n = fmt_addr(s, sizeof(s), g_fs.myaddr);
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
		uint32_t need = d.len + (c->state == CONV_ANNOUNCED ? 6 : 0);

		if (count < need) {
			return -EMSGSIZE;
		}
		uint32_t k = 0;

		if (c->state == CONV_ANNOUNCED) {
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
		if (!c || c->state != CONV_CONNECTED) {
			return -ENOTCONN;
		}
		if (count > AETHER_MAX_MSG) {
			return -EMSGSIZE;
		}
		int ret = aether_mesh_send_reliable(g_fs.iface, c->peer, buf, count,
						    AETHER_NET_RETRIES);

		return ret < 0 ? ret : (int)count;
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

	/* Clunking a conversation's ctl or dir tears the conversation down
	 * (frees the slot + wakes a blocked data reader). */
	if (an->conv && (an->kind == K_CTL || an->kind == K_CONVDIR)) {
		k_mutex_lock(&g_fs.lock, K_FOREVER);
		conv_free(an->conv);
		k_mutex_unlock(&g_fs.lock);
	}
	return 0;
}

static const struct ninep_fs_ops aether_net_ops = {
	.get_root = anet_get_root,
	.walk = anet_walk,
	.open = anet_open,
	.read = anet_read,
	.write = anet_write,
	.stat = anet_stat,
	.clunk = anet_clunk,
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
	link_child(&g_fs.root, &g_fs.clone);
	link_child(&g_fs.root, &g_fs.topstatus);
	link_child(&g_fs.root, &g_fs.addr);

	int ret = aether_mesh_register_recv_callback(iface, aether_recv_cb, NULL);

	LOG_INF("/net/aether datagram service up (%d conversations); recv_cb=%d",
		AETHER_MAX_CONNS, ret);
	return ret;
}
