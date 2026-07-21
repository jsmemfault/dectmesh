# `mount aether!<addr>` — mounting a remote mesh node's filesystem on the deck

Deck-side recipe for browsing a **remote** node's 9P filesystem over the mesh,
using `transport_nsfile` (9p4z `b29043f`). This is the on-deck equivalent of the
host tool `aether_conv --bridge`.

## The pieces (all already on the deck)

- `bind l2cap!dect-modem` → the modem's `/net/aether` conversation interface in
  the deck's namespace (you already do this).
- `transport_nsfile` (NEW, `CONFIG_NINEP_TRANSPORT_NSFILE=y`) — runs a
  `ninep_client` over an open, message-framed namespace channel.
- `ninep_client` + `fs_9p` (`FS_TYPE_9P` + `struct ninep_mount_ctx`) + `ns_mount`
  — mount that client's tree at a path (already used for `l2cap!`).
- The far end: the target node's **mesh 9P server** (`aether_9p_mesh.c`) answers
  9P over reliable mesh unicast — proven working, mesh now hardened (v0.7.29).

## The dialer flow (`aether!<addr>` → a mount)

`aether!<addr>` = clone a conversation, connect it to `<addr>` (reliable unicast,
so its `data` node tunnels 9P to that node's mesh 9P server), then mount that
channel. Reference sequence — wire this into your zrc dial resolver:

```c
#include <zephyr/9p/transport_nsfile.h>
#include <zephyr/9p/client.h>
#include <zephyr/namespace/namespace.h>
#include <zephyr/namespace/fs_9p.h>

/* one per concurrent aether mount */
static K_THREAD_STACK_DEFINE(aether_rx_stack, 2048);
static struct ninep_transport_nsfile aether_tp;
static struct ninep_client          aether_client;
static struct ninep_mount_ctx       aether_mctx;
static struct fs_mount_t            aether_fsmnt;

int aether_dial_mount(const char *addr /* "00:00:00:00:20:00" */, const char *mntpt)
{
	char n[16], path[48], cmd[40];
	int cfd, dfd, ret, conv;

	/* 1. clone a conversation; read its number N (clone lives under the bound
	 *    /net/aether from `bind l2cap!dect-modem`). HOLD cfd open -- clunking it
	 *    frees the conversation upstream. */
	cfd = ns_open("/net/aether/clone", FS_O_RDWR);
	if (cfd < 0) return cfd;
	ret = ns_read(cfd, n, sizeof(n) - 1); if (ret <= 0) { ns_close(cfd); return -EIO; }
	n[ret] = 0; conv = atoi(n);

	/* 2. connect the conversation to <addr> (CONV_CONNECTED, reliable). The ctl
	 *    node is the clone fid itself; write "connect <addr>" to it. */
	snprintf(cmd, sizeof(cmd), "connect %s", addr);
	if (ns_write(cfd, cmd, strlen(cmd)) < 0) { ns_close(cfd); return -EIO; }

	/* 3. open the data channel -- this is the bidirectional 9P pipe to <addr>. */
	snprintf(path, sizeof(path), "/net/aether/%d/data", conv);
	dfd = ns_open(path, FS_O_RDWR);
	if (dfd < 0) { ns_close(cfd); return dfd; }

	/* 4. transport over the data channel; client over the transport. Clamp msize
	 *    to the mesh datagram MTU so no 9P message is split (one msg == one
	 *    datagram). */
	ret = ninep_transport_nsfile_init(&aether_tp, dfd, 448,
					  aether_rx_stack, K_THREAD_STACK_SIZEOF(aether_rx_stack));
	if (ret) { ns_close(dfd); ns_close(cfd); return ret; }

	static const struct ninep_client_config cc = {
		.max_message_size = 512, .version = "9P2000", .timeout_ms = 5000,
	};
	ret = ninep_client_init(&aether_client, &cc, &aether_tp.transport); /* calls start() */
	if (ret) return ret;
	ret = ninep_client_version(&aether_client); if (ret) return ret;

	/* 5. mount the remote root into the namespace. fs_9p attaches via the client;
	 *    walk/read/etc. now tunnel over the mesh to <addr>'s server. */
	aether_mctx.client = &aether_client;
	aether_mctx.aname[0] = 0;              /* attach at the remote root */
	aether_fsmnt.type = FS_TYPE_9P;
	aether_fsmnt.fs_data = &aether_mctx;
	return ns_mount(&aether_fsmnt, mntpt, 0);
	/* keep cfd (the clone/ctl fid) held for the mount's lifetime -- do NOT clunk
	 * it, or the upstream conversation is freed and the data channel dies. */
}
```

Then in the shell: `aether!20:00` → `aether_dial_mount("00:00:00:00:20:00", "/n/remote")`,
and `ls /n/remote` walks the remote node's `/net`+`/dev` across the mesh.

## Notes / gotchas

- **Hold the clone fid** (`cfd`) for the mount's lifetime; clunking it frees the
  conversation upstream (the recurring stateful-fid rule — it's why the host
  `aether_conv` holds fids and one-shot `9p` can't). Unmount = clunk data, then clone.
- **msize == one datagram** (~448 B). Fine for state files; bulk transfer wants a
  windowing layer (future).
- **Connected (reliable) vs broadcast:** use `connect <addr>` (unicast, ARQ) for a
  mount, not the `ff:ff:ff:ff:ff:ff` party line. The ARQ + the v0.7.29 retry-on-busy
  make the channel robust; if the RX thread ever desyncs on a duplicate/late reply,
  the 9p4z client's tag muxing should still route it — add a tag-match filter in
  `transport_nsfile` only if testing shows stale replies leaking through.
- **Cross-thread fds (verified):** the transport's RX thread `ns_read`s the fd
  your dialer opened, and client threads `ns_write` it. The ns fd table is global
  so this works; the non-owner warning was downgraded to debug (9p4z 4e1b505). No
  action needed -- just don't `ns_close` the data fd from your dialer while the
  mount is live (the transport owns it once started; it closes on stop).
- **Reformation tolerance (backlog #3b):** if the mesh reforms mid-session, a
  reliable send can transiently find no route. Once #3b lands (hold-and-retry
  across a route change), a mount survives the heal; until then a mount may error
  during a reform and need a remount.

## Status
`transport_nsfile` is committed (9p4z b29043f) but **not yet hardware-tested** —
it needs a deck build with `CONFIG_NINEP_TRANSPORT_NSFILE=y`. First test: mount a
2-hop node and `ls` its root (matches the host `mesh9p_demo.sh` result).
