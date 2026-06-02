/*
 * Copyright (c) 2026 Æther Authors
 * SPDX-License-Identifier: MIT
 *
 * 9P server presenting the Æther mesh as /net/aether. The generators read the
 * live mesh context in-process, so the files are real, not a stub: reading
 * /net/aether/tree returns this node's HONR position right now, and writing
 * /net/aether/chat broadcasts a party-line message across the mesh.
 */

#include "aether_9p.h"

#ifdef CONFIG_NINEP

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/net/honr.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/sysfs.h>
#include <zephyr/9p/transport_uart.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

LOG_MODULE_REGISTER(aether_9p, LOG_LEVEL_INF);

/* Mesh context, owned by aether_mesh.c. */
extern struct aether_mesh_ctx *g_mesh_ctx;

#define NINEP_UART_NODE DT_CHOSEN(zephyr_ninep_uart)

static struct ninep_transport transport;
static struct ninep_server server;
static struct ninep_sysfs sysfs;
static struct ninep_sysfs_entry sysfs_entries[16];
static uint8_t rx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static struct net_if *g_iface;

/* Copy a NUL-terminated string slice into the read buffer honoring @p offset. */
static int emit(uint8_t *buf, size_t buf_size, uint64_t offset, const char *s)
{
	size_t slen = strlen(s);
	size_t n;

	if (offset >= slen) {
		return 0;
	}
	n = MIN(slen - (size_t)offset, buf_size);
	memcpy(buf, s + offset, n);
	return (int)n;
}

/* ---- single-value files ---- */

static int gen_addr(uint8_t *buf, size_t sz, uint64_t off, void *ctx)
{
	char s[8];

	ARG_UNUSED(ctx);
	snprintf(s, sizeof(s), "%04x\n", g_mesh_ctx ? g_mesh_ctx->honr_addr : 0);
	return emit(buf, sz, off, s);
}

static int gen_rank(uint8_t *buf, size_t sz, uint64_t off, void *ctx)
{
	char s[8];

	ARG_UNUSED(ctx);
	snprintf(s, sizeof(s), "%u\n", g_mesh_ctx ? honr_rank(g_mesh_ctx->honr_addr) : 0);
	return emit(buf, sz, off, s);
}

static int gen_parent(uint8_t *buf, size_t sz, uint64_t off, void *ctx)
{
	char s[16] = "none\n";

	ARG_UNUSED(ctx);
	if (g_mesh_ctx && g_mesh_ctx->honr_joined && !honr_is_root(g_mesh_ctx->honr_addr)) {
		snprintf(s, sizeof(s), "%04x\n", honr_parent(g_mesh_ctx->honr_addr));
	}
	return emit(buf, sz, off, s);
}

static int gen_nodeid(uint8_t *buf, size_t sz, uint64_t off, void *ctx)
{
	char s[16];

	ARG_UNUSED(ctx);
	snprintf(s, sizeof(s), "%08x\n", aether_honr_node_id());
	return emit(buf, sz, off, s);
}

/* ---- multi-line files (rendered into a static buffer at offset 0) ---- */

static int gen_tree(uint8_t *buf, size_t sz, uint64_t off, void *ctx)
{
	static char tb[256];
	struct aether_mesh_ctx *c = g_mesh_ctx;

	ARG_UNUSED(ctx);
	if (off == 0) {
		uint16_t a = c ? c->honr_addr : 0;
		uint8_t rank = honr_rank(a);
		int p = snprintf(tb, sizeof(tb), "node %04x\nrank %u\nstate %s\nnodeid %08x\n",
				 a, rank,
				 honr_is_root(a) ? "root" :
				 (c && c->honr_joined ? "joined" : "unjoined"),
				 aether_honr_node_id());

		if (c && c->honr_joined && !honr_is_root(a)) {
			p += snprintf(tb + p, sizeof(tb) - p, "parent %04x\n", honr_parent(a));
		}
		if (c) {
			for (unsigned int n = HONR_CHILD_MIN; n <= HONR_CHILD_MAX; n++) {
				if (c->honr_child_bitmap & BIT(n)) {
					p += snprintf(tb + p, sizeof(tb) - p, "child %04x\n",
						      honr_set_nibble(a, rank, (uint8_t)n));
				}
			}
		}
	}
	return emit(buf, sz, off, tb);
}

static int gen_neighbors(uint8_t *buf, size_t sz, uint64_t off, void *ctx)
{
	static char nb[512];
	struct aether_mesh_ctx *c = g_mesh_ctx;

	ARG_UNUSED(ctx);
	if (off == 0) {
		int p = 0;

		nb[0] = '\0';
		for (int i = 0; c && i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
			if (!c->neighbors[i].active) {
				continue;
			}
			uint32_t age = (k_uptime_get_32() - c->neighbors[i].last_seen) / 1000U;

			p += snprintf(nb + p, sizeof(nb) - p, "%02x:%02x rssi %d age %us\n",
				      c->neighbors[i].addr[4], c->neighbors[i].addr[5],
				      c->neighbors[i].rssi, age);
		}
	}
	return emit(buf, sz, off, nb);
}

static int gen_routes(uint8_t *buf, size_t sz, uint64_t off, void *ctx)
{
	static char rb[512];
	struct aether_mesh_ctx *c = g_mesh_ctx;

	ARG_UNUSED(ctx);
	if (off == 0) {
		int p = 0;

		rb[0] = '\0';
		for (int i = 0; c && i < CONFIG_AETHER_MAX_ROUTES; i++) {
			if (!c->routes[i].active) {
				continue;
			}
			p += snprintf(rb + p, sizeof(rb) - p, "%02x:%02x via %02x:%02x hops %d\n",
				      c->routes[i].dest[4], c->routes[i].dest[5],
				      c->routes[i].next_hop[4], c->routes[i].next_hop[5],
				      c->routes[i].hop_count);
		}
	}
	return emit(buf, sz, off, rb);
}

/* ---- party-line chat: read the log, write to broadcast ---- */

#define CHAT_LOG_SIZE 1024
static char chat_log[CHAT_LOG_SIZE];
static size_t chat_log_len;
static struct k_mutex chat_mutex;
static const uint8_t aether_bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

void aether_9p_chat_log(const uint8_t src[6], const uint8_t *data, size_t len)
{
	char line[96];
	int n = snprintf(line, sizeof(line), "<%02x:%02x> %.*s\n",
			 src[4], src[5], (int)len, (const char *)data);

	if (n <= 0) {
		return;
	}
	k_mutex_lock(&chat_mutex, K_FOREVER);
	if (chat_log_len + (size_t)n >= CHAT_LOG_SIZE) {
		/* Ring down: keep the newest half. */
		size_t keep = CHAT_LOG_SIZE / 2;

		memmove(chat_log, chat_log + (chat_log_len - keep), keep);
		chat_log_len = keep;
	}
	memcpy(chat_log + chat_log_len, line, (size_t)n);
	chat_log_len += (size_t)n;
	k_mutex_unlock(&chat_mutex);
}

static int gen_chat(uint8_t *buf, size_t sz, uint64_t off, void *ctx)
{
	int r;

	ARG_UNUSED(ctx);
	k_mutex_lock(&chat_mutex, K_FOREVER);
	if (off >= chat_log_len) {
		r = 0;
	} else {
		size_t n = MIN(chat_log_len - (size_t)off, sz);

		memcpy(buf, chat_log + off, n);
		r = (int)n;
	}
	k_mutex_unlock(&chat_mutex);
	return r;
}

static int write_chat(const uint8_t *buf, uint32_t count, uint64_t off, void *ctx)
{
	uint32_t n = count;
	int ret;

	ARG_UNUSED(off);
	ARG_UNUSED(ctx);
	if (!g_iface) {
		return -ENODEV;
	}
	/* A trailing newline from `echo` is not part of the message. */
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
		n--;
	}
	if (n == 0) {
		return count;
	}
	ret = aether_mesh_send(g_iface, aether_bcast, buf, n, AETHER_PRIORITY_NORMAL);
	if (ret < 0) {
		LOG_WRN("9P chat send failed: %d", ret);
		return ret;
	}
	return count;
}

static int register_tree(void)
{
	int ret;

	ret = ninep_sysfs_register_dir(&sysfs, "net");
	if (ret < 0 && ret != -EEXIST) {
		return ret;
	}
	ret = ninep_sysfs_register_dir(&sysfs, "net/aether");
	if (ret < 0) {
		return ret;
	}
	ninep_sysfs_register_file(&sysfs, "net/aether/addr", gen_addr, NULL);
	ninep_sysfs_register_file(&sysfs, "net/aether/rank", gen_rank, NULL);
	ninep_sysfs_register_file(&sysfs, "net/aether/parent", gen_parent, NULL);
	ninep_sysfs_register_file(&sysfs, "net/aether/nodeid", gen_nodeid, NULL);
	ninep_sysfs_register_file(&sysfs, "net/aether/tree", gen_tree, NULL);
	ninep_sysfs_register_file(&sysfs, "net/aether/neighbors", gen_neighbors, NULL);
	ninep_sysfs_register_file(&sysfs, "net/aether/routes", gen_routes, NULL);
	ninep_sysfs_register_writable_file(&sysfs, "net/aether/chat", gen_chat, write_chat, NULL);
	return 0;
}

int aether_9p_init(struct net_if *iface)
{
	const struct device *uart = DEVICE_DT_GET(NINEP_UART_NODE);
	struct ninep_transport_uart_config tc = {
		.uart_dev = uart,
		.rx_buf = rx_buf,
		.rx_buf_size = sizeof(rx_buf),
	};
	struct ninep_server_config sc = {
		.fs_ops = ninep_sysfs_get_ops(),
		.fs_ctx = &sysfs,
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000",
	};
	int ret;

	g_iface = iface;
	k_mutex_init(&chat_mutex);

	if (!device_is_ready(uart)) {
		LOG_ERR("9P UART device not ready");
		return -ENODEV;
	}

	ret = ninep_sysfs_init(&sysfs, sysfs_entries, ARRAY_SIZE(sysfs_entries));
	if (ret < 0) {
		LOG_ERR("sysfs init: %d", ret);
		return ret;
	}
	ret = register_tree();
	if (ret < 0) {
		LOG_ERR("/net/aether registration: %d", ret);
		return ret;
	}
	ret = ninep_transport_uart_init(&transport, &tc, NULL, NULL);
	if (ret < 0) {
		LOG_ERR("UART transport: %d", ret);
		return ret;
	}
	ret = ninep_server_init(&server, &sc, &transport);
	if (ret < 0) {
		LOG_ERR("9P server init: %d", ret);
		return ret;
	}
	ret = ninep_server_start(&server);
	if (ret < 0) {
		LOG_ERR("9P server start: %d", ret);
		return ret;
	}

	LOG_INF("9P server up: /net/aether over %s", uart->name);
	return 0;
}

#endif /* CONFIG_NINEP */
