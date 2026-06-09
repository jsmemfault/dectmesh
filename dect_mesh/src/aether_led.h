/*
 * RGB LED mesh-state indicator (Thingy:91 X nRF9151 RGB LED).
 *
 *   blue          = HONR root (0x0000)
 *   green         = joined child
 *   amber blink   = unjoined / electing / orphaned
 *   white flash   = datagram received (party-line / /net/aether activity)
 */
#ifndef AETHER_LED_H_
#define AETHER_LED_H_

#include <zephyr/net/net_if.h>

/* Start the mesh-state LED. Reads HONR state from g_mesh_ctx and registers a
 * recv callback (the mesh supports several) for the RX-activity flash. */
int aether_led_init(struct net_if *iface);

#endif /* AETHER_LED_H_ */
