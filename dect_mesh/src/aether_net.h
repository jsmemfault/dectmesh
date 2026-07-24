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

#endif /* AETHER_NET_H_ */
