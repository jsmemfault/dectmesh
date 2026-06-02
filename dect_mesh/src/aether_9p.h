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

#ifdef CONFIG_NINEP
/** @brief Start the 9P server serving /net/aether over the chosen UART. */
int aether_9p_init(struct net_if *iface);

/** @brief Append a received party-line message to the /net/aether/chat log. */
void aether_9p_chat_log(const uint8_t src[6], const uint8_t *data, size_t len);
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
#endif /* CONFIG_NINEP */

#endif /* AETHER_9P_H_ */
