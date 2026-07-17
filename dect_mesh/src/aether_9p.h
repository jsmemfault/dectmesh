/*
 * Copyright (c) 2026 Æther Authors
 * SPDX-License-Identifier: MIT
 *
 * 9P server exposing the Æther mesh as a /net/aether synthetic filesystem.
 * Runs in-process on the node that owns the mesh (the nRF9151), served over a
 * UART transport. A companion relay (e.g. the Thingy:91X nRF5340 bridging
 * L2CAP<->UART) carries it to clients; the tree it serves is identical whatever
 * the transport, so a client can bind it over the local radio or the relay.
 */

#ifndef AETHER_9P_H_
#define AETHER_9P_H_

#include <zephyr/net/net_if.h>
#include <stdint.h>
#include <stddef.h>

struct ninep_server_config;

#ifdef CONFIG_NINEP
/** @brief Start the 9P server serving /net/aether over the chosen UART. */
int aether_9p_init(struct net_if *iface);

/**
 * @brief Start the second 9P server serving the same namespace over the MESH:
 * reliable unicast datagrams as the transport, one datagram per 9P message.
 * Lets a peer (or a host driving a peer's conversation layer) mount this
 * node's filesystem across the mesh, through relays.
 */
int aether_9p_mesh_init(struct net_if *iface,
			const struct ninep_server_config *base_sc);

/** @brief Append a received party-line message to the /net/aether/chat log. */
void aether_9p_chat_log(const uint8_t src[6], const uint8_t *data, size_t len);

/**
 * @brief Copy the party-line chat scrollback (the same rolling buffer served
 * over /net/aether/chat) into a caller buffer, for the `aether chatlog` shell
 * command -- lets the console see history, not just messages that arrived
 * while it happened to be watching.
 *
 * @param out Destination buffer
 * @param outsz Destination size
 * @return Number of bytes copied (<= outsz)
 */
size_t aether_9p_chat_log_snapshot(char *out, size_t outsz);
#else
static inline int aether_9p_init(struct net_if *iface)
{
	(void)iface;
	return 0;
}
static inline void aether_9p_chat_log(const uint8_t src[6], const uint8_t *data, size_t len)
{
	(void)src;
	(void)data;
	(void)len;
}
static inline size_t aether_9p_chat_log_snapshot(char *out, size_t outsz)
{
	(void)out;
	(void)outsz;
	return 0;
}
#endif /* CONFIG_NINEP */

#endif /* AETHER_9P_H_ */
