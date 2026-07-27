/*
 * /net/aether -- the Plan 9 /net/udp-style reliable-datagram interface to the
 * Aether mesh, as a custom 9P fs_ops (see aether_net.c + doc/NET_AETHER_SPEC.md).
 * Mounted under the 9151's 9P server via union_fs alongside the sysfs /dev tree.
 */
#ifndef AETHER_NET_H_
#define AETHER_NET_H_

#include <zephyr/net/net_if.h>
#include <zephyr/9p/server.h>

/* Bring up the datagram service: register the mesh recv callback + build the
 * static /net/aether tree (clone/status/addr). myaddr is this node's 6-byte
 * mesh address (reported via addr/local). */
int aether_net_init(struct net_if *iface, const uint8_t myaddr[6]);

/* fs_ops + context for union_fs_mount(&u, "/net/aether", ops, ctx). */
const struct ninep_fs_ops *aether_net_get_ops(void);
void *aether_net_get_ctx(void);

/* Datagram-service counters for fleet telemetry: whole datagrams sent/received
 * and conversation-rxq-full drops (the back-pressure signal). Any pointer NULL. */
void aether_net_get_stats(uint32_t *data_tx, uint32_t *data_rx, uint32_t *rxq_drops);

/* Drain a mesh peer's dev/mflt over the air, IN-PROCESS (no uart1 round-trips) --
 * runs a full 9P client session to the peer's mesh 9P server over a conversation.
 * Writes the peer's self-describing DEV:/MC: stream into out (cap); returns bytes
 * written (>=0) or negative errno. Used by dev/mflt_mesh to relay every in-range
 * peer's telemetry in one inter-chip read. */
int aether_net_drain_peer(const uint8_t peer_eui[6], uint8_t *out, size_t cap);

/* Fast, bounded single-Tversion probe of a peer's mesh 9P server. Writes a
 * one-line human summary (send-result, get-result, reply type) into out; returns
 * bytes written. Returns in ~2s even against a silent peer, so a 9P read can
 * carry the result back before the client times out. Diagnostic for dev/mflt_mesh. */
int aether_net_probe_peer(const uint8_t peer_eui[6], char *out, size_t cap);

#endif /* AETHER_NET_H_ */
