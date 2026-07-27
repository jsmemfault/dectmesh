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
#include <zephyr/9p/dfu.h>
#include <zephyr/9p/union_fs.h>
#include "aether_net.h"
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#if defined(CONFIG_MEMFAULT)
#include <memfault/core/data_packetizer.h>
#endif

LOG_MODULE_REGISTER(aether_9p, LOG_LEVEL_INF);

/* Mesh context, owned by aether_mesh.c. */
extern struct aether_mesh_ctx *g_mesh_ctx;

#define NINEP_UART_NODE DT_CHOSEN(zephyr_ninep_uart)

static struct ninep_transport transport;
static struct ninep_server server;
static struct ninep_sysfs sysfs;
static struct ninep_sysfs_entry sysfs_entries[24];

/* union_fs composes the namespace: sysfs at "/" (the /dev tree, incl. the
 * re-homed /dev/aether node-state files) + the aether_net datagram fs at
 * "/net/aether". The server serves the union. */
static struct ninep_union_fs aether_union;
static struct ninep_union_mount aether_union_mounts[2];
static struct ninep_dfu dfu;
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

			/* addr (HONR/short, mutable) · identity (node_eui, durable,
			 * all-zero=unknown) · real RSSI/SNR from the PHY · age. For
			 * peer discovery: connect by addr now, match by identity
			 * across re-joins. */
			p += snprintf(nb + p, sizeof(nb) - p,
				      "%02x:%02x identity %02x:%02x:%02x:%02x:%02x:%02x"
				      " rssi %d snr %d age %us\n",
				      c->neighbors[i].addr[4], c->neighbors[i].addr[5],
				      c->neighbors[i].node_eui[0], c->neighbors[i].node_eui[1],
				      c->neighbors[i].node_eui[2], c->neighbors[i].node_eui[3],
				      c->neighbors[i].node_eui[4], c->neighbors[i].node_eui[5],
				      c->neighbors[i].rssi, c->neighbors[i].snr, age);
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

size_t aether_9p_chat_log_snapshot(char *out, size_t outsz)
{
	size_t n;

	k_mutex_lock(&chat_mutex, K_FOREVER);
	n = MIN(chat_log_len, outsz);
	memcpy(out, chat_log, n);
	k_mutex_unlock(&chat_mutex);
	return n;
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

/* ---- Firmware-as-files: /dev/firmware (this 9151's own MCUboot DFU) ----
 *
 * ninep_dfu streams a signed image into the MCUboot secondary slot, which on
 * the Thingy:91 X 9151 lives in the external GD25LE255E QSPI flash
 * (PM_EXTERNAL_FLASH_MCUBOOT_SECONDARY). Closing the file requests a TEST
 * upgrade; writing dev/confirm makes the booted image permanent (else MCUboot
 * reverts on the next reboot). The 5340 aggregator re-exports this node as
 * /dev/fw9151 via union_fs. The image is a single merged ns image
 * (TFM_MCUBOOT_IMAGE_NUMBER=1), matching ninep_dfu's single-slot model.
 */
static int dfu_write_reboot(const uint8_t *buf, uint32_t count, uint64_t off, void *ctx)
{
	ARG_UNUSED(buf); ARG_UNUSED(off); ARG_UNUSED(ctx);
	LOG_INF("reboot requested via 9P");
	k_sleep(K_MSEC(100)); /* let logs flush */
	sys_reboot(SYS_REBOOT_COLD);
	return count; /* unreached */
}

static int dfu_write_confirm(const uint8_t *buf, uint32_t count, uint64_t off, void *ctx)
{
	ARG_UNUSED(buf); ARG_UNUSED(off); ARG_UNUSED(ctx);
	int ret = ninep_dfu_confirm();

	if (ret == 0) {
		LOG_INF("9151 image confirmed via 9P");
	}
	return ret < 0 ? ret : count;
}

static void dfu_status(enum ninep_dfu_state state, uint32_t bytes, int err)
{
	ARG_UNUSED(bytes);
	/* Quiesce the DECT mesh while firmware streams over the inter-chip UART, so
	 * the low-priority flash-write thread isn't starved by RX/HONR and a chunk's
	 * Rwrite doesn't miss the relay's timeout ("write failed"). Released on
	 * complete/error; the node reboots to apply. This is the "RF must never block
	 * a wired OTA" guarantee. See aether_mesh_set_dfu_active. */
	switch (state) {
	case NINEP_DFU_ERASING:
		aether_mesh_set_dfu_active(true);
		LOG_INF("fw9151 DFU: erasing secondary slot (external flash); mesh quiesced");
		break;
	case NINEP_DFU_RECEIVING:
		aether_mesh_set_dfu_active(true);  /* in case ERASING was skipped */
		break;
	case NINEP_DFU_COMPLETE:
		aether_mesh_set_dfu_active(false);
		LOG_INF("fw9151 DFU: complete - reboot to apply; mesh resumed");
		break;
	case NINEP_DFU_ERROR:
		aether_mesh_set_dfu_active(false);
		LOG_ERR("fw9151 DFU: error %d; mesh resumed", err);
		break;
	default: break;
	}
}

static int register_dev(void)
{
	int ret;

	ret = ninep_sysfs_register_dir(&sysfs, "dev");
	if (ret < 0 && ret != -EEXIST) {
		return ret;
	}
	(void)ninep_sysfs_register_writable_file(&sysfs, "dev/reboot",
						 NULL, dfu_write_reboot, NULL);
	(void)ninep_sysfs_register_writable_file(&sysfs, "dev/confirm",
						 NULL, dfu_write_confirm, NULL);

	struct ninep_dfu_config dfu_cfg = {
		.path = "dev/firmware",
		.status_cb = dfu_status,
	};
	ret = ninep_dfu_init(&dfu, &sysfs, &dfu_cfg);
	if (ret < 0) {
		LOG_ERR("fw9151 DFU init: %d", ret);
		return ret;
	}

	if (!boot_is_img_confirmed()) {
		LOG_WRN("9151 image not confirmed - write /dev/confirm or it reverts on reboot");
	}
	return 0;
}

#if defined(CONFIG_MEMFAULT)
/* Base64-encode n bytes of `in` into `out` (no NUL); returns bytes written. */
static int b64enc(const uint8_t *in, size_t n, char *out)
{
	static const char t[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int o = 0;

	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16 |
			     (uint32_t)(i + 1 < n ? in[i + 1] : 0) << 8 |
			     (uint32_t)(i + 2 < n ? in[i + 2] : 0);
		out[o++] = t[(v >> 18) & 63];
		out[o++] = t[(v >> 12) & 63];
		out[o++] = (i + 1 < n) ? t[(v >> 6) & 63] : '=';
		out[o++] = (i + 2 < n) ? t[v & 63] : '=';
	}
	return o;
}

/*
 * dev/mflt -- drain pending Memfault chunks as base64 "MC:<b64>:" lines (the
 * standard chunk-export wire format). Reading the file consumes chunks; a host
 * or gateway forwarder (tools/mflt_forward.sh) reads it over 9P and POSTs each
 * chunk to Memfault. Observability rides the same filesystem as everything else
 * -- no separate telemetry protocol, and it works over UART / USB / BLE / mesh.
 */
static int gen_mflt(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);
	if (offset || !memfault_packetizer_data_available()) {
		return 0;   /* one drain per read op; empty when nothing pending */
	}
	int total = 0;
	uint8_t chunk[192];

	/* Self-describing: which Memfault device these chunks belong to. The serial
	 * is this node's CGA, matching the id set in dect_metrics.c -- so one generic
	 * forwarder can POST any node's chunks without being told who it is. */
	if (g_mesh_ctx) {
		const uint8_t *e = g_mesh_ctx->node_eui;

		total += snprintf((char *)buf, buf_size,
				  "DEV:dect-%02x%02x%02x%02x%02x%02x:\n",
				  e[0], e[1], e[2], e[3], e[4], e[5]);
	}

	while (memfault_packetizer_data_available()) {
		/* get_chunk is destructive, so ensure the encoded line fits first:
		 * "MC:" + ceil(n/3)*4 + ":\n" for n<=192 is <= 264 bytes. */
		if (total + 270 > (int)buf_size) {
			break;
		}
		size_t clen = sizeof(chunk);

		if (!memfault_packetizer_get_chunk(chunk, &clen)) {
			break;
		}
		buf[total++] = 'M'; buf[total++] = 'C'; buf[total++] = ':';
		total += b64enc(chunk, clen, (char *)buf + total);
		buf[total++] = ':'; buf[total++] = '\n';
	}
	return total;
}

/*
 * dev/mflt_mesh -- relay EVERY in-range mesh peer's dev/mflt in ONE inter-chip
 * read. For each active neighbor (addressed by its durable node_eui) this drains
 * the peer's Memfault chunks OVER THE AIR, in-process (aether_net_drain_peer ->
 * a mesh 9P client on a conversation, so no uart1 round-trips per op), and
 * concatenates the self-describing DEV:/MC: streams. The gateway's relay/host
 * then pulls the whole mesh's telemetry with a single read of this file instead
 * of the host tunneling a 9P session per peer over uart1 (the load that saturated
 * the inter-chip link). Bounded by peer count + a wall-clock budget so the read
 * never blocks the relay too long; peers not reached this read come next cycle.
 */
static int gen_mflt_mesh(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);
	if (offset || !g_mesh_ctx) {
		return 0;   /* one drain per read op */
	}
	static const uint8_t zero_eui[6] = { 0 };
	uint32_t start = k_uptime_get_32();
	int total = 0, drained = 0, checked = 0;

	int eligible = 0;
	for (int i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
		if (!g_mesh_ctx->neighbors[i].active) continue;
		checked++;
		if (memcmp(g_mesh_ctx->neighbors[i].node_eui, zero_eui, 6) != 0)
			eligible++;
	}
	LOG_INF("mflt_mesh read: %d active, %d eligible, cap=%d",
		checked, eligible, (int)buf_size);

	/* Always emit a header so the read is self-diagnosing even when a small
	 * client msize or a stuck drain would otherwise yield an empty, silent read
	 * (the 9151 console is DTR-flaky, so this is the only reliable channel). */
	if (buf_size > 48) {
		total += snprintf((char *)buf + total, buf_size - total,
			"MESHDBG active=%d eligible=%d cap=%d\n",
			checked, eligible, (int)buf_size);
	}

	for (int i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS && drained < 3; i++) {
		if (!g_mesh_ctx->neighbors[i].active) {
			continue;
		}
		struct aether_neighbor *nb = &g_mesh_ctx->neighbors[i];

		if (memcmp(nb->node_eui, zero_eui, 6) == 0) {
			continue;   /* null identity == the self entry; skip */
		}
		if ((int)buf_size - total < 128) {
			break;      /* not enough room for even a DBG line; next read */
		}
		if (k_uptime_get_32() - start > 10000) {
			break;      /* time budget: don't block the relay read too long */
		}
		/* Address by durable node_eui (the CGA identity) -- same as the host
		 * `aether_conv --bridge` and the rest of the stack; the conversation layer
		 * resolves it to the current HONR route. Self already skipped (null eui). */
		extern int g_drain_sr, g_drain_gr;
		g_drain_sr = -99; g_drain_gr = -99;
		int n = aether_net_drain_peer(nb->node_eui, buf + total, buf_size - total);

		if (n > 0) {
			total += n;
			drained++;
		} else if ((int)buf_size - total > 64) {
			/* Diagnostic in-band (9151 console is DTR-flaky): send/recv result. */
			total += snprintf((char *)buf + total, buf_size - total,
				"DBG %02x%02x%02x%02x%02x%02x: n=%d sr=%d gr=%d\n",
				nb->node_eui[0], nb->node_eui[1], nb->node_eui[2],
				nb->node_eui[3], nb->node_eui[4], nb->node_eui[5],
				n, g_drain_sr, g_drain_gr);
		}
	}
	return total;
}

/*
 * dev/mflt_probe -- fast diagnostic: for each eligible neighbor, do ONE bounded
 * Tversion RPC to its mesh 9P server and report send/get/reply-type. Returns in a
 * few seconds even against silent peers (unlike a full drain), so the read reliably
 * carries the answer back. Answers: is the peer reachable at the transport (sr>=0),
 * and does its mesh 9P server actually reply (gr==0, rtype==101/Rversion)?
 */
static int gen_mflt_probe(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);
	if (offset || !g_mesh_ctx) {
		return 0;
	}
	static const uint8_t zero_eui[6] = { 0 };
	int total = 0, probed = 0;

	for (int i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS && probed < 3; i++) {
		if (!g_mesh_ctx->neighbors[i].active) {
			continue;
		}
		struct aether_neighbor *nb = &g_mesh_ctx->neighbors[i];

		if (memcmp(nb->node_eui, zero_eui, 6) == 0) {
			continue;
		}
		if ((int)buf_size - total < 96) {
			break;
		}
		total += aether_net_probe_peer(nb->node_eui,
			(char *)buf + total, buf_size - total);
		probed++;
	}
	if (probed == 0 && buf_size > 32) {
		total += snprintf((char *)buf + total, buf_size - total,
			"PROBE: no eligible neighbors\n");
	}
	return total;
}
#endif /* CONFIG_MEMFAULT */

/*
 * Node-state files under /dev/aether (spec §10: node state -- addr/rank/tree/
 * neighbors/routes/chat -- is distinct from the /net/aether *network* datagram
 * interface, which is served by aether_net.c and union-mounted at /net/aether).
 * Requires "dev" to exist (register_dev runs first).
 */
static int register_dev_aether(void)
{
	int ret = ninep_sysfs_register_dir(&sysfs, "dev/aether");

	if (ret < 0 && ret != -EEXIST) {
		return ret;
	}
	ninep_sysfs_register_file(&sysfs, "dev/aether/addr", gen_addr, NULL);
	ninep_sysfs_register_file(&sysfs, "dev/aether/rank", gen_rank, NULL);
	ninep_sysfs_register_file(&sysfs, "dev/aether/parent", gen_parent, NULL);
	ninep_sysfs_register_file(&sysfs, "dev/aether/nodeid", gen_nodeid, NULL);
	ninep_sysfs_register_file(&sysfs, "dev/aether/tree", gen_tree, NULL);
	ninep_sysfs_register_file(&sysfs, "dev/aether/neighbors", gen_neighbors, NULL);
	ninep_sysfs_register_file(&sysfs, "dev/aether/routes", gen_routes, NULL);
	ninep_sysfs_register_writable_file(&sysfs, "dev/aether/chat", gen_chat, write_chat, NULL);
#if defined(CONFIG_MEMFAULT)
	/* Memfault chunk export as a file -- read to drain, forward to the cloud. */
	ninep_sysfs_register_file(&sysfs, "dev/mflt", gen_mflt, NULL);
	/* ... and every in-range mesh peer's chunks, drained over the air in one read. */
	ninep_sysfs_register_file(&sysfs, "dev/mflt_mesh", gen_mflt_mesh, NULL);
	/* Fast diagnostic: single-Tversion probe per peer (reachability + reply). */
	ninep_sysfs_register_file(&sysfs, "dev/mflt_probe", gen_mflt_probe, NULL);
#endif
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
		.fs_ops = ninep_union_fs_get_ops(),   /* serve the composed namespace */
		.fs_ctx = &aether_union,
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
	ret = register_dev();              /* dev/ + firmware/reboot/confirm */
	if (ret < 0) {
		LOG_ERR("/dev/firmware registration: %d", ret);
		return ret;
	}
	ret = register_dev_aether();       /* dev/aether/* node-state files */
	if (ret < 0) {
		LOG_ERR("/dev/aether registration: %d", ret);
		return ret;
	}

	/* Bring up the /net/aether datagram service (registers the mesh recv cb +
	 * builds its tree), then compose: sysfs at "/" + aether_net at "/net/aether". */
	static const uint8_t zero_addr[6];
	const uint8_t *myaddr = g_mesh_ctx ? g_mesh_ctx->local_addr : zero_addr;

	ret = aether_net_init(iface, myaddr);
	if (ret < 0) {
		LOG_ERR("/net/aether init: %d", ret);
		return ret;
	}
	ret = ninep_union_fs_init(&aether_union, aether_union_mounts,
				  ARRAY_SIZE(aether_union_mounts));
	if (ret < 0) {
		LOG_ERR("union_fs init: %d", ret);
		return ret;
	}
	ret = ninep_union_fs_mount(&aether_union, "/", ninep_sysfs_get_ops(), &sysfs);
	if (ret < 0) {
		LOG_ERR("union mount /: %d", ret);
		return ret;
	}
	ret = ninep_union_fs_mount(&aether_union, "/net/aether",
				   aether_net_get_ops(), aether_net_get_ctx());
	if (ret < 0) {
		LOG_ERR("union mount /net/aether: %d", ret);
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

	/* Second server instance: the same namespace served over the mesh
	 * itself (reliable unicast datagrams as the 9P transport). Non-fatal
	 * if it fails -- the UART/USB path stays up either way. */
	ret = aether_9p_mesh_init(iface, &sc);
	if (ret < 0) {
		LOG_WRN("9P-over-mesh server not started: %d", ret);
	}
	return 0;
}

#endif /* CONFIG_NINEP */
