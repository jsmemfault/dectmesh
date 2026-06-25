/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/heymac.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/net/honr.h>
#include "aether_led.h"
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(aether_basic, LOG_LEVEL_INF);

#include <string.h>
#include <stdio.h>
#include <zephyr/shell/shell_uart.h>

#include "aether_9p.h"
#include "dect_metrics.h"

/* Mesh context, for detailed status (defined in aether_mesh.c). */
extern struct aether_mesh_ctx *g_mesh_ctx;

static struct net_if *aether_iface;

/* Per-node shell prompt, e.g. "ae 1a:2b> ", set once the address is known. */
static char node_prompt[16];

static const char *heymac_state_name(enum heymac_state state)
{
	switch (state) {
	case HEYMAC_STATE_INITIAL:      return "INITIAL";
	case HEYMAC_STATE_INITIALIZING: return "INITIALIZING";
	case HEYMAC_STATE_LURKING:      return "LURKING";
	case HEYMAC_STATE_BEACONING:    return "BEACONING";
	case HEYMAC_STATE_LINKING:      return "LINKING";
	default:                        return "?";
	}
}

/* Party-line broadcast address: every node receives and re-floods. */
static const uint8_t aether_bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/* Party-line chat receive callback: print each incoming message to the
 * console. Runs in the mesh RX thread; printk is used so the line appears
 * regardless of which shell instance (if any) issued a command.
 */
static void chat_recv_cb(struct net_if *iface, const uint8_t src[6],
			 const uint8_t *data, size_t len, bool broadcast, void *user_data)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(user_data);

	/* The party line is broadcast-only; a unicast datagram is not chat. */
	if (!broadcast) {
		return;
	}

	/* Identify the talker by the last two address bytes. */
	printk("\n<chat %02x:%02x> %.*s\n", src[4], src[5], (int)len, (const char *)data);

	/* Mirror into /net/aether/chat for 9P readers (no-op without CONFIG_NINEP). */
	aether_9p_chat_log(src, data, len);
}

static void print_status(void)
{
	struct heymac_addr addr;
	int net_id;
	int neighbor_count, route_count;

	if (!aether_iface) {
		LOG_ERR("Æther interface not available");
		return;
	}

	/* Get HeyMac status */
	if (heymac_get_addr(aether_iface, &addr) == 0) {
		if (heymac_addr_is_short(&addr)) {
			LOG_INF("Local address: %02x:%02x (short)",
				addr.short_addr[0], addr.short_addr[1]);
		} else {
			LOG_INF("Local address: %02x:%02x:%02x:%02x:%02x:%02x (long)",
				addr.long_addr[0], addr.long_addr[1], addr.long_addr[2],
				addr.long_addr[3], addr.long_addr[4], addr.long_addr[5]);
		}
	}

	net_id = heymac_get_network_id(aether_iface);
	if (net_id >= 0) {
		LOG_INF("Network ID: 0x%04x", net_id);
	}

	/* Get mesh status */
	neighbor_count = aether_mesh_get_neighbor_count(aether_iface);
	route_count = aether_mesh_get_route_count(aether_iface);
	
	LOG_INF("Mesh status: %d neighbors, %d routes", neighbor_count, route_count);
}

static void periodic_hello(void)
{
	if (aether_iface) {
		aether_mesh_send_hello(aether_iface);
		LOG_DBG("Periodic hello sent");
	}
}

int main(void)
{
	int ret;
	uint32_t hello_count = 0;

	/* Very early debug to test logging */
	printk("=== MAIN STARTING ===\n");
	LOG_INF("Æther Basic Node Starting");

	/* Wait for network interface to be available */
	aether_iface = net_if_get_default();
	if (!aether_iface) {
		LOG_ERR("No network interface available");
		
		/* List all available interfaces for debugging */
		int count = 0;
		STRUCT_SECTION_FOREACH(net_if, iface) {
			LOG_INF("Found interface %d: %p", count, iface);
			count++;
		}
		LOG_INF("Total interfaces found: %d", count);
		
		return -ENODEV;
	}
	
	LOG_INF("Found default interface: %p", aether_iface);

	/* Bring up the network interface */
	LOG_INF("Bringing up network interface...");
	net_if_up(aether_iface);
	
	/* Wait for interface to be up */
	int attempts = 0;
	while (!net_if_is_up(aether_iface) && attempts < 20) {
		LOG_INF("Waiting for network interface... (attempt %d)", attempts + 1);
		k_sleep(K_MSEC(500));
		attempts++;
	}
	
	if (!net_if_is_up(aether_iface)) {
		LOG_ERR("Failed to bring up network interface after %d attempts", attempts);
		return -ENETDOWN;
	}

	LOG_INF("Network interface is up");

	/* Initialize Æther mesh */
	ret = aether_mesh_init(aether_iface, 0x915E);  /* Use default network ID */
	if (ret < 0) {
		LOG_ERR("Failed to initialize Æther mesh: %d", ret);
		return ret;
	}

	LOG_INF("Æther mesh network ready");

	/* Print received party-line messages to the console. */
	aether_mesh_register_recv_callback(aether_iface, chat_recv_cb, NULL);
	LOG_INF("Party-line chat ready: type 'aether chat <message>'");

	/* Expose the mesh as a /net/aether 9P tree (no-op unless CONFIG_NINEP). */
	if (aether_9p_init(aether_iface) == 0 && IS_ENABLED(CONFIG_NINEP)) {
		LOG_INF("9P server ready: /net/aether");
	}

	/* RGB LED mesh-state indicator (blue=root, green=child, amber=unjoined). */
	aether_led_init(aether_iface);

	/* Memfault: device identity + mesh heartbeat metrics (no-op without it). */
	dect_metrics_init();

	/* Tag the shell prompt with this node's address so the three terminals
	 * are easy to tell apart. The tag matches the talker ID shown in chat.
	 */
	if (g_mesh_ctx) {
		const struct shell *sh = shell_backend_uart_get_ptr();

		snprintf(node_prompt, sizeof(node_prompt), "ae %02x:%02x> ",
			 g_mesh_ctx->local_addr[4], g_mesh_ctx->local_addr[5]);
		if (sh) {
			shell_prompt_change(sh, node_prompt);
		}
	}

	/* Print initial status */
	print_status();

	/* Main application loop */
	while (1) {
		/* Print status every 10 hello intervals */
		if ((hello_count % 10) == 0) {
			print_status();
		}

		/* Send periodic hello */
		periodic_hello();
		hello_count++;

		/* Sleep until next hello interval */
		k_sleep(K_SECONDS(CONFIG_AETHER_HELLO_INTERVAL));
	}

	return 0;
}

/* Shell commands for testing */
#ifdef CONFIG_SHELL

static int cmd_aether_status(const struct shell *sh, size_t argc, char **argv)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	struct heymac_context *hctx;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!aether_iface || !ctx) {
		shell_error(sh, "Æther interface not available");
		return -ENODEV;
	}

	shell_print(sh, "=== Æther Node Status ===");
	shell_print(sh, "  addr   %02x:%02x:%02x:%02x:%02x:%02x",
		    ctx->local_addr[0], ctx->local_addr[1], ctx->local_addr[2],
		    ctx->local_addr[3], ctx->local_addr[4], ctx->local_addr[5]);
	shell_print(sh, "  net_id 0x%04x", ctx->mesh_net_id);

	hctx = net_if_l2_data(aether_iface);
	if (hctx) {
		shell_print(sh, "  state  %s", heymac_state_name(hctx->state));
		shell_print(sh, "  rx/tx  %u/%u pkts, %u/%u err",
			    hctx->rx_packets, hctx->tx_packets,
			    hctx->rx_errors, hctx->tx_errors);
		shell_print(sh, "  beacon tx %u, rx %u", hctx->tx_beacons, hctx->rx_beacons);
	}

	/* Neighbors with link quality and recency. */
	shell_print(sh, "  neighbors %d", aether_mesh_get_neighbor_count(aether_iface));
	for (int i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
		if (ctx->neighbors[i].active) {
			uint32_t age = (k_uptime_get_32() - ctx->neighbors[i].last_seen) / 1000;

			shell_print(sh, "    %02x:%02x:%02x:%02x:%02x:%02x  rssi %d  %us ago",
				    ctx->neighbors[i].addr[0], ctx->neighbors[i].addr[1],
				    ctx->neighbors[i].addr[2], ctx->neighbors[i].addr[3],
				    ctx->neighbors[i].addr[4], ctx->neighbors[i].addr[5],
				    ctx->neighbors[i].rssi, age);
		}
	}

	/* Routes. */
	shell_print(sh, "  routes %d", aether_mesh_get_route_count(aether_iface));
	for (int i = 0; i < CONFIG_AETHER_MAX_ROUTES; i++) {
		if (ctx->routes[i].active) {
			shell_print(sh, "    %02x:%02x:%02x:%02x:%02x:%02x  via %02x:%02x  hops %d",
				    ctx->routes[i].dest[0], ctx->routes[i].dest[1],
				    ctx->routes[i].dest[2], ctx->routes[i].dest[3],
				    ctx->routes[i].dest[4], ctx->routes[i].dest[5],
				    ctx->routes[i].next_hop[4], ctx->routes[i].next_hop[5],
				    ctx->routes[i].hop_count);
		}
	}

	shell_print(sh, "  hello  sent %u, recv %u", ctx->hello_sent, ctx->hello_received);
	shell_print(sh, "  fwd %u, drop %u", ctx->packets_forwarded, ctx->packets_dropped);

	return 0;
}

static int cmd_aether_hello(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!aether_iface) {
		shell_error(sh, "Æther interface not available");
		return -ENODEV;
	}

	int ret = aether_mesh_send_hello(aether_iface);
	if (ret < 0) {
		shell_error(sh, "Failed to send hello: %d", ret);
		return ret;
	}

	shell_print(sh, "Hello message sent");
	return 0;
}

static int cmd_aether_send(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t dst[6];
	const char *message;
	int ret;

	if (argc != 3) {
		shell_error(sh, "Usage: aether send <dst_addr> <message>");
		shell_print(sh, "Example: aether send 01:02:03:04:05:06 \"Hello World\"");
		return -EINVAL;
	}

	if (!aether_iface) {
		shell_error(sh, "Æther interface not available");
		return -ENODEV;
	}

	/* Parse destination address */
	ret = sscanf(argv[1], "%02x:%02x:%02x:%02x:%02x:%02x",
		     (unsigned int *)&dst[0], (unsigned int *)&dst[1], 
		     (unsigned int *)&dst[2], (unsigned int *)&dst[3],
		     (unsigned int *)&dst[4], (unsigned int *)&dst[5]);
	if (ret != 6) {
		shell_error(sh, "Invalid address format. Use: XX:XX:XX:XX:XX:XX");
		return -EINVAL;
	}

	message = argv[2];
	if (strlen(message) > CONFIG_AETHER_MAX_PAYLOAD) {
		shell_error(sh, "Message too long (max %d bytes)", CONFIG_AETHER_MAX_PAYLOAD);
		return -EMSGSIZE;
	}

	/* Send mesh message */
	ret = aether_mesh_send(aether_iface, dst, (const uint8_t *)message, 
			       strlen(message), AETHER_PRIORITY_NORMAL);
	if (ret < 0) {
		shell_error(sh, "Failed to send message: %d", ret);
		return ret;
	}

	shell_print(sh, "Message sent to %02x:%02x:%02x:%02x:%02x:%02x",
		    dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]);
	return 0;
}

static int cmd_aether_info(const struct shell *sh, size_t argc, char **argv)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	struct heymac_context *hctx;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!aether_iface || !ctx) {
		shell_error(sh, "Æther interface not available");
		return -ENODEV;
	}

	hctx = net_if_l2_data(aether_iface);

	shell_print(sh, "=== Æther Parameters ===");
	shell_print(sh, "  addr        %02x:%02x:%02x:%02x:%02x:%02x",
		    ctx->local_addr[0], ctx->local_addr[1], ctx->local_addr[2],
		    ctx->local_addr[3], ctx->local_addr[4], ctx->local_addr[5]);
	shell_print(sh, "  net_id      0x%04x", ctx->mesh_net_id);
	shell_print(sh, "  mac state   %s", hctx ? heymac_state_name(hctx->state) : "?");
#if defined(CONFIG_AETHER_ROUTING_HONR)
	shell_print(sh, "  honr addr   %04x (%s)", ctx->honr_addr,
		    ctx->honr_joined ? "joined" : "unjoined");
#endif
	shell_print(sh, "  ttl         %d hops", CONFIG_AETHER_DEFAULT_TTL);
	shell_print(sh, "  hello int   %d s", CONFIG_AETHER_HELLO_INTERVAL);
	shell_print(sh, "  beacon int  %d ms", CONFIG_HEYMAC_BEACON_INTERVAL_MS);
	shell_print(sh, "  nbr timeout %d s", CONFIG_AETHER_NEIGHBOR_TIMEOUT);
	shell_print(sh, "  route tmout %d s", CONFIG_AETHER_ROUTE_TIMEOUT);
	shell_print(sh, "  max payload %d bytes", CONFIG_AETHER_MAX_PAYLOAD);
	shell_print(sh, "  max nbrs    %d", CONFIG_AETHER_MAX_NEIGHBORS);
	shell_print(sh, "  max routes  %d", CONFIG_AETHER_MAX_ROUTES);

	return 0;
}

static int cmd_aether_chat(const struct shell *sh, size_t argc, char **argv)
{
	char msg[CONFIG_AETHER_MAX_PAYLOAD + 1];
	size_t off = 0;
	int ret;

	if (argc < 2) {
		shell_error(sh, "Usage: aether chat <message>");
		return -EINVAL;
	}

	if (!aether_iface) {
		shell_error(sh, "Æther interface not available");
		return -ENODEV;
	}

	/* Rejoin the shell-split words into one message. */
	for (size_t i = 1; i < argc; i++) {
		size_t word_len = strlen(argv[i]);

		if (off + word_len + (i > 1 ? 1 : 0) >= sizeof(msg)) {
			shell_error(sh, "Message too long (max %d bytes)",
				    CONFIG_AETHER_MAX_PAYLOAD);
			return -EMSGSIZE;
		}
		if (i > 1) {
			msg[off++] = ' ';
		}
		memcpy(&msg[off], argv[i], word_len);
		off += word_len;
	}
	msg[off] = '\0';

	/* Party-line: broadcast to every node in the mesh. */
	ret = aether_mesh_send(aether_iface, aether_bcast, (const uint8_t *)msg, off,
			       AETHER_PRIORITY_NORMAL);
	if (ret < 0) {
		shell_error(sh, "chat send failed: %d", ret);
		return ret;
	}

	shell_print(sh, "<you> %s", msg);
	return 0;
}

#if defined(CONFIG_AETHER_ROUTING_HONR)
/* Consolidated view of this node's place in the HONR tree: its address, depth
 * (rank), parent, and the children it has handed addresses to. Run it on every
 * node to assemble the whole tree by hand.
 */
static int cmd_aether_tree(const struct shell *sh, size_t argc, char **argv)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	uint16_t addr;
	uint8_t rank;
	int children = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!aether_iface || !ctx) {
		shell_error(sh, "Æther interface not available");
		return -ENODEV;
	}

	addr = ctx->honr_addr;
	rank = honr_rank(addr);

	shell_print(sh, "=== HONR Tree (local view) ===");
	shell_print(sh, "  node     %04x  rank %u (depth)  %s", addr, rank,
		    honr_is_root(addr) ? "ROOT" :
		    (ctx->honr_joined ? "joined" : "unjoined"));
	shell_print(sh, "  node_id  %08x  (lowest id wins the root election)",
		    aether_honr_node_id());

	if (honr_is_root(addr)) {
		shell_print(sh, "  parent   (none - root)");
	} else if (ctx->honr_joined) {
		shell_print(sh, "  parent   %04x", honr_parent(addr));
	} else {
		shell_print(sh, "  parent   (none - not joined yet)");
	}

	for (unsigned int n = HONR_CHILD_MIN; n <= HONR_CHILD_MAX; n++) {
		if (ctx->honr_child_bitmap & BIT(n)) {
			shell_print(sh, "  child    %04x",
				    honr_set_nibble(addr, rank, (uint8_t)n));
			children++;
		}
	}
	if (children == 0) {
		shell_print(sh, "  children (none)");
	} else {
		shell_print(sh, "  children %d of %d slots used", children,
			    (int)(HONR_CHILD_MAX - HONR_CHILD_MIN + 1));
	}

	return 0;
}
#endif /* CONFIG_AETHER_ROUTING_HONR */

SHELL_STATIC_SUBCMD_SET_CREATE(aether_cmds,
	SHELL_CMD_ARG(info, NULL, "Show Æther parameters and MAC state", cmd_aether_info, 1, 0),
	SHELL_CMD_ARG(status, NULL, "Show Æther node status", cmd_aether_status, 1, 0),
#if defined(CONFIG_AETHER_ROUTING_HONR)
	SHELL_CMD_ARG(tree, NULL, "Show HONR tree position (parent/children/rank)",
		      cmd_aether_tree, 1, 0),
#endif
	SHELL_CMD_ARG(hello, NULL, "Send hello message", cmd_aether_hello, 1, 0),
	SHELL_CMD_ARG(chat, NULL, "Broadcast a party-line message", cmd_aether_chat, 2, 20),
	SHELL_CMD_ARG(send, NULL, "Send mesh message to <addr>", cmd_aether_send, 3, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(aether, &aether_cmds, "Æther mesh commands", NULL);

#endif /* CONFIG_SHELL */