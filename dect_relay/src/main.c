/*
 * Copyright (c) 2026 Jon Sharp
 * SPDX-License-Identifier: MIT
 *
 * DECT mesh BLE relay (Thingy:91 X nRF5340).
 *
 * A transparent byte relay between a Bluetooth LE L2CAP connection-oriented
 * channel (CoC, no GATT) and the inter-chip UART to the nRF9151. The nRF9151
 * serves /net/aether over 9P on that UART (plan C); this image lets a client
 * mount it over BLE. The relay does not parse 9P — it just pumps bytes both
 * ways, so the 9P endpoints handle their own framing/auth (factotum).
 *
 *   cyberdeck  --9P/L2CAP-->  [nRF5340 relay]  --9P/UART-->  [nRF9151 server]
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/net_buf.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/session_pool_uart.h>
#include <zephyr/9p/session_pool_l2cap.h>
#include <zephyr/9p/sysfs.h>
#include <zephyr/9p/union_fs.h>
#include <zephyr/9p/remote_fs.h>
#include <zephyr/9p/dfu.h>
#include <zephyr/9p/client.h>
#include <zephyr/9p/transport_uart.h>
#include <zephyr/9p/protocol.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(dect_relay, CONFIG_DECT_RELAY_LOG_LEVEL);

/* LE L2CAP SPSM in the dynamic range (0x0080-0x00FF). 0x0001-0x007F are
 * SIG-fixed and rejected by stricter stacks (e.g. iOS CoreBluetooth) for a
 * custom channel, so we avoid 9p4z's 0x0009 default. The client must match.
 */
#define RELAY_PSM         0x0080
#define RELAY_MTU         247
#define UART_RING_SIZE    2048
#define L2CAP_CHUNK_MAX   244   /* <= MTU, leave room for SDU overhead */

/* Inter-chip UART to the nRF9151 (its uart1, carrying 9P /net/aether). */
static const struct device *const ic_uart = DEVICE_DT_GET(DT_NODELABEL(uart1));

/*
 * nRF9151 reset line, owned by the nRF5340. On the Thingy:91 X the 9151's
 * nRESET is wired to the 5340's P1.07 (cf. the connectivity_bridge thingy91x
 * overlay: reset-gpios = <&gpio1 7>). It is active-low. There is NO external
 * pull-up on this rail (verified by halting the 5340 and reading the pad:
 * leaving P1.07 hi-Z/DISCONNECTED floats it and the 9151 never comes out of
 * reset). The connectivity_bridge gets away with DISCONNECTED only because its
 * SWD-probe (swdp-gpio) driver actively holds the line; we have no such driver,
 * so we must DRIVE the line: low to reset, high to release/hold-released.
 * Nothing else drives it, so on a cold power-up the 9151 stays dead until we
 * pulse it -- exactly why /dev/fw9151 found a silent 9151 after a power-cycle.
 * (Before, it only ran because a J-Link session on SW2->nRF91 had released it.)
 * We are the interface MCU now; bringing the 9151 up is our job.
 */
static const struct device *const gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
#define NRF91_RESET_PIN 7

/* Pulse the 9151 out of reset so it (re)boots its 9P server with our uart1
 * already initialized: assert (low), hold, then DRIVE high to release. */
static void nrf91_reset(void)
{
	if (!device_is_ready(gpio1)) {
		LOG_ERR("gpio1 not ready; cannot reset the 9151");
		return;
	}
	gpio_pin_configure(gpio1, NRF91_RESET_PIN, GPIO_OUTPUT_LOW);
	k_sleep(K_MSEC(100));
	gpio_pin_configure(gpio1, NRF91_RESET_PIN, GPIO_OUTPUT_HIGH);
	LOG_INF("released 9151 reset (P1.0%d driven high); 9151 booting",
		NRF91_RESET_PIN);
}

/* Probe the 9151's nRESET (P1.07) natural level WITHOUT driving it, to learn
 * whether the line is released (external pull-up -> the 9151 POR-boots on its
 * own) or held. Leaves the pin as a high-Z input so the 9151 can POR-boot
 * unmolested -- the theory being our reset PULSE was breaking a 9151 that boots
 * fine by itself. */
static void nrf91_probe(void)
{
	if (!device_is_ready(gpio1)) {
		LOG_ERR("gpio1 not ready");
		return;
	}
	gpio_pin_configure(gpio1, NRF91_RESET_PIN, GPIO_INPUT);
	k_busy_wait(2000);
	int v = gpio_pin_get_raw(gpio1, NRF91_RESET_PIN);

	LOG_INF("9151 nRESET P1.0%d natural level=%d (1=pull-up/released, 0=held)",
		NRF91_RESET_PIN, v);
}

/* ---- nRF9151 console <-> USB CDC ACM bridge (phase 1) ----
 *
 * The 9151's console/shell (its uart0) lands on the 5340's uart0. We pump it
 * verbatim to a USB CDC ACM port so the host can drive the 9151 shell
 * (dect bands/info/cad) and read mesh logs -- replacing the console path lost
 * when this relay displaced the connectivity bridge. Fully interrupt-driven:
 * each endpoint's ISR drains RX into the ring headed for the peer's TX, and
 * fills its own TX from the ring the peer fed. No extra thread.
 */
static const struct device *const cons_uart = DEVICE_DT_GET(DT_NODELABEL(uart0));
static const struct device *const cdc_uart  = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

#define CONS_RING_SIZE 1024
RING_BUF_DECLARE(cons_to_host_rb, CONS_RING_SIZE); /* 9151 console -> host */
RING_BUF_DECLARE(host_to_cons_rb, CONS_RING_SIZE); /* host -> 9151 console */

struct bridge_ep {
	const struct device *self;
	const struct device *peer;
	struct ring_buf *rx_ring; /* bytes read from self queue here (-> peer TX) */
	struct ring_buf *tx_ring; /* bytes written to self drain from here */
	bool peer_needs_dtr;      /* peer is a CDC ACM: only pump when host attached */
};
static struct bridge_ep cons_ep; /* self = uart0 (9151 console) */
static struct bridge_ep cdc_ep;  /* self = CDC ACM (host)       */

static void bridge_isr(const struct device *dev, void *user_data)
{
	struct bridge_ep *ep = user_data;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			/* If the peer is a CDC ACM with no host attached (DTR low),
			 * drop the data instead of backing up its TX endpoint -- that
			 * backup floods usb_transfer "No transfer slot available" and
			 * starves the other CDC ports (e.g. the 9P server on cdc1). */
			if (ep->peer_needs_dtr) {
				uint32_t dtr = 0;
				(void)uart_line_ctrl_get(ep->peer, UART_LINE_CTRL_DTR, &dtr);
				if (!dtr) {
					uint8_t drop[64];
					(void)uart_fifo_read(dev, drop, sizeof(drop));
					continue;
				}
			}
			uint8_t *dst;
			uint32_t space = ring_buf_put_claim(ep->rx_ring, &dst, 64);

			if (space == 0) {
				/* peer not draining; discard to keep the FIFO moving */
				uint8_t drop[64];
				(void)uart_fifo_read(dev, drop, sizeof(drop));
				(void)ring_buf_put_finish(ep->rx_ring, 0);
			} else {
				int n = uart_fifo_read(dev, dst, space);
				(void)ring_buf_put_finish(ep->rx_ring, n < 0 ? 0 : n);
				if (n > 0) {
					uart_irq_tx_enable(ep->peer);
				}
			}
		}
		if (uart_irq_tx_ready(dev)) {
			uint8_t *src;
			uint32_t avail = ring_buf_get_claim(ep->tx_ring, &src, 64);

			if (avail == 0) {
				(void)ring_buf_get_finish(ep->tx_ring, 0);
				uart_irq_tx_disable(dev);
			} else {
				int wrote = uart_fifo_fill(dev, src, avail);
				(void)ring_buf_get_finish(ep->tx_ring, wrote < 0 ? 0 : wrote);
			}
		}
	}
}

static void console_bridge_init(void)
{
	cons_ep = (struct bridge_ep){ .self = cons_uart, .peer = cdc_uart,
				      .rx_ring = &cons_to_host_rb,
				      .tx_ring = &host_to_cons_rb,
				      .peer_needs_dtr = true };  /* peer = CDC: gate on host DTR */
	cdc_ep = (struct bridge_ep){ .self = cdc_uart, .peer = cons_uart,
				     .rx_ring = &host_to_cons_rb,
				     .tx_ring = &cons_to_host_rb,
				     .peer_needs_dtr = false }; /* peer = real uart0, always ready */

	uart_irq_callback_user_data_set(cons_uart, bridge_isr, &cons_ep);
	uart_irq_callback_user_data_set(cdc_uart, bridge_isr, &cdc_ep);
	uart_irq_rx_enable(cons_uart);
	uart_irq_rx_enable(cdc_uart);
}

/* ---- Firmware-as-files: 9P session pool on cdc_acm_uart1 (phase 2) ----
 *
 * A 9p4z server exposing the 5340's own MCUboot DFU as /dev/fw5340, plus
 * /dev/confirm and /dev/reboot, over a dedicated USB CDC ACM port. The host can
 * `9p write /dev/fw5340 <signed.bin>` to self-update the 5340 in situ, then
 * write /dev/confirm to make it permanent (else MCUboot reverts).
 *
 * The CDC stream is served through a DTR-gated session pool: each host
 * open/close of the port allocates/frees a fresh session (RX state + fid
 * namespace), so the server no longer wedges on client reconnect the way a
 * single long-lived stream server did. Phase 3 folds in a 9P client link to the
 * 9151 + union_fs to re-export /dev/fw9151 + /net/aether onto the same shared
 * fs_ops, served by both this pool and an L2CAP session pool. The existing
 * uart1<->L2CAP byte-pump is untouched, so /net/aether-over-BLE keeps working.
 */
static const struct device *const fw_cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart1));
NINEP_SESSION_POOL_UART_DEFINE(fw_uart_pool, 1, CONFIG_NINEP_MAX_MESSAGE_SIZE);
/* Second pool serving the SAME fw_sysfs over BLE L2CAP CoC (PSM RELAY_PSM) -- the
 * same /dev tree, a second transport. This is the 9P transport-agnostic story made
 * literal: USB and BLE expose one filesystem with no per-transport logic. */
NINEP_SESSION_POOL_L2CAP_DEFINE(fw_l2cap_pool, 1, CONFIG_NINEP_MAX_MESSAGE_SIZE);
static struct ninep_sysfs fw_sysfs;
static struct ninep_sysfs_entry fw_sysfs_entries[48];
static struct ninep_dfu fw_dfu;

/*
 * The served namespace is composed with union_fs (like the 9151's):
 *   "/"           -> fw_sysfs   (the local /dev tree: fw5340, fw9151, link9151, ...)
 *   "/net/aether" -> remote_fs  (the 9151's dynamic datagram tree, proxied live
 *                                over the mesh client -- clone/ctl/data
 *                                conversations, per doc/NET_AETHER_SPEC.md)
 * Both session pools (USB-CDC + L2CAP) serve this union, so /net/aether reaches
 * host and BLE 9P clients alike.
 */
static struct ninep_union_fs fw_union;
static struct ninep_union_mount fw_union_mounts[2];
static struct ninep_remote_fs aether_rfs;
/* One node per concurrently-walked fid under /net/aether. The 9151 caps at
 * AETHER_MAX_CONNS conversations; each uses a handful of fids (clone/ctl/data/
 * dir/status) plus dir-listing walks -- 24 covers all four with headroom. */
static struct ninep_remote_node aether_rnodes[24];

static int fw_write_reboot(const uint8_t *buf, uint32_t count, uint64_t off, void *ctx)
{
	ARG_UNUSED(buf); ARG_UNUSED(off); ARG_UNUSED(ctx);
	LOG_INF("reboot requested via 9P");
	k_sleep(K_MSEC(100)); /* let logs flush */
	sys_reboot(SYS_REBOOT_COLD);
	return count; /* unreached */
}

static int fw_write_confirm(const uint8_t *buf, uint32_t count, uint64_t off, void *ctx)
{
	ARG_UNUSED(buf); ARG_UNUSED(off); ARG_UNUSED(ctx);
	int ret = ninep_dfu_confirm();

	if (ret == 0) {
		LOG_INF("5340 image confirmed via 9P");
	}
	return ret < 0 ? ret : count;
}

static void fw_dfu_status(enum ninep_dfu_state state, uint32_t bytes, int err)
{
	ARG_UNUSED(bytes);
	switch (state) {
	case NINEP_DFU_ERASING:  LOG_INF("fw5340 DFU: erasing secondary slot"); break;
	case NINEP_DFU_COMPLETE: LOG_INF("fw5340 DFU: complete - reboot to apply"); break;
	case NINEP_DFU_ERROR:    LOG_ERR("fw5340 DFU: error %d", err); break;
	default: break;
	}
}

/* ---- 9P CLIENT to the nRF9151 over uart1 (the aggregator link) ----
 *
 * The 5340 is now a 9P CLIENT to the 9151's server (which exposes /net/aether
 * and /dev/firmware over its uart0 <-> our uart1), replacing the old
 * uart1<->L2CAP byte-pump. Because the 5340 now PARSES 9P, it can re-export
 * selected 9151 nodes into its own served tree -- here the 9151's /dev/firmware
 * is proxied as /dev/fw9151, so the same host `9p` tooling that flashes
 * /dev/fw5340 can flash the 9151 too. The client transport is polling-mode
 * (its recv callback broadcasts a condvar, which must run in thread context).
 */
static struct ninep_transport mesh_transport;
static struct ninep_client mesh_client;
static const struct ninep_client_config mesh_client_cfg = {
	.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
	.version = "9P2000",
	/* The 9151's 9P server is a lowest-priority busy-poll thread, starved by
	 * the DECT mesh (RX windows, HONR), so its reply latency is bursty -- the
	 * RX bytes arrive but sometimes just past a tight deadline (seen: isr got
	 * all 19 Rversion bytes yet the 5s version still timed out). Be generous. */
	.timeout_ms = 20000,
};
static uint32_t mesh_root_fid;
static bool mesh_attached;
/*
 * Two fine-grained locks instead of one coarse client lock (the 9p4z client is
 * now per-tag concurrency-safe, so it doesn't need external serialization):
 *  - mesh_sess: guards mesh_attached + mesh_root_fid. Held only BRIEFLY (snapshot
 *    the root / (re)attach), NOT during the client walk/open/read/write -- so a
 *    blocking /net/aether read holds no lock and can't freeze other ops.
 *  - mesh_wr:   serializes the single OTA write stream (fw9151_writing/wfid/woff)
 *    + the auto-confirm armed/staged state.
 * Lock order when both are held: mesh_wr -> mesh_sess (only fw9151_write nests
 * them, via fw9151_open_remote). Nothing takes mesh_sess then mesh_wr.
 */
static struct k_mutex mesh_sess;
static struct k_mutex mesh_wr;

/* --- 9151 link health, exposed read-only via dev/link9151 --- */
static volatile uint32_t mesh_last_contact_ms; /* uptime of last successful handshake/op */
static volatile uint32_t mesh_relink_attempts; /* consecutive failed relinks since last up */
static volatile uint32_t mesh_relink_total;    /* lifetime successful (re)links */
#define MESH_MON_STACK 2048
static K_THREAD_STACK_DEFINE(mesh_mon_stack, MESH_MON_STACK);
static struct k_thread mesh_mon_thread;

static inline void mesh_note_contact(void) { mesh_last_contact_ms = k_uptime_get_32(); }

/* --- auto-confirm a freshly-swapped 9151 image once the link verifies it --- */
static bool fw9151_autoconfirm = true;  /* default on; toggle via dev/fw9151auto */
static bool fw9151_armed;               /* an image was staged; confirm once it boots */
static uint8_t fw9151_staged_major, fw9151_staged_minor;
static uint16_t fw9151_staged_rev;
static bool fw9151_staged_valid;        /* parsed a valid MCUboot header from the stream */
static void fw9151_auto_confirm_check(void);  /* defined after the fw9151 proxy ops */

/*
 * Custom RX transport for the client on uart1.
 *
 * transport_uart's polling mode (uart_poll_in) does NOT deliver RX on the nRF53
 * uart1 (it works on the nRF91 server side, but not here), and its interrupt
 * mode calls recv_cb from the ISR -- unsafe for the client, whose recv_cb
 * (client_recv_callback) broadcasts a condvar (thread context only). So: drain
 * the UART in the ISR (proven by the old byte-pump), accumulate a full 9P
 * message, then hand it to the client's recv_cb on a work queue. While a message
 * is in flight to the work queue we hold off new bytes (the client is strictly
 * request/response, so nothing new arrives until the reply is consumed).
 */
static uint8_t mesh_rx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];   /* ISR accumulator */
static uint8_t mesh_rx_msg[CONFIG_NINEP_MAX_MESSAGE_SIZE];   /* handed to recv_cb */
static size_t mesh_rx_len;
static uint32_t mesh_rx_expected;
static bool mesh_rx_have_hdr;
static volatile bool mesh_rx_processing;
static uint32_t mesh_rx_proc_len;
static volatile uint32_t mesh_rx_isr_calls;   /* diag: ISR invocations */
static volatile uint32_t mesh_rx_isr_bytes;   /* diag: bytes drained from uart1 RX */
static uint8_t mesh_rx_dbg[48];               /* diag: first bytes since last reset */
static volatile uint32_t mesh_rx_dbg_len;
static volatile uint32_t mesh_rx_arrive_ms;   /* diag: uptime when a complete msg was framed */
static struct k_work mesh_rx_work;
#define MESH_RX_WQ_STACK 2048
static K_THREAD_STACK_DEFINE(mesh_rx_wq_stack, MESH_RX_WQ_STACK);
static struct k_work_q mesh_rx_wq;

static void mesh_rx_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	uint32_t len = mesh_rx_proc_len;

	/* Copy the completed reply out, then free the ISR accumulator + clear
	 * 'processing' BEFORE invoking recv_cb. recv_cb broadcasts the condvar,
	 * which wakes the caller to send its NEXT request -- whose reply can land
	 * immediately, so the ISR must already be able to accept it (otherwise it
	 * gets dropped and the next op times out). recv_cb reads the copy, so the
	 * ISR refilling mesh_rx_buf in parallel is safe. */
	memcpy(mesh_rx_msg, mesh_rx_buf, len);
	mesh_rx_len = 0;
	mesh_rx_have_hdr = false;
	mesh_rx_expected = 0;
	mesh_rx_processing = false;

	LOG_DBG("mesh RX deliver: size=%u type=%u tag=%u len=%u",
		(unsigned)(mesh_rx_msg[0] | (mesh_rx_msg[1] << 8) |
			   (mesh_rx_msg[2] << 16) | (mesh_rx_msg[3] << 24)),
		mesh_rx_msg[4],
		(unsigned)(mesh_rx_msg[5] | (mesh_rx_msg[6] << 8)), len);

	if (mesh_transport.recv_cb) {
		mesh_transport.recv_cb(&mesh_transport, mesh_rx_msg, len,
				       mesh_transport.user_data);
	}
}

static void mesh_uart_isr(const struct device *dev, void *ud)
{
	ARG_UNUSED(ud);

	if (!uart_irq_update(dev)) {
		return;
	}
	mesh_rx_isr_calls++;
	while (uart_irq_rx_ready(dev)) {
		uint8_t b[64];
		int n = uart_fifo_read(dev, b, sizeof(b));

		if (n <= 0) {
			break;
		}
		mesh_rx_isr_bytes += n;
		for (int j = 0; j < n; j++) {
			if (mesh_rx_dbg_len < sizeof(mesh_rx_dbg)) {
				mesh_rx_dbg[mesh_rx_dbg_len++] = b[j];
			}
		}
		if (mesh_rx_processing) {
			continue;  /* reply still being handed up; drop (none expected) */
		}
		for (int i = 0; i < n; i++) {
			if (mesh_rx_len >= sizeof(mesh_rx_buf)) {
				mesh_rx_len = 0;
				mesh_rx_have_hdr = false;
			}
			mesh_rx_buf[mesh_rx_len++] = b[i];

			if (!mesh_rx_have_hdr && mesh_rx_len >= 7) {
				mesh_rx_expected = mesh_rx_buf[0] |
						   (mesh_rx_buf[1] << 8) |
						   (mesh_rx_buf[2] << 16) |
						   (mesh_rx_buf[3] << 24);
				if (mesh_rx_expected < 7 ||
				    mesh_rx_expected > sizeof(mesh_rx_buf)) {
					mesh_rx_len = 0;  /* resync */
					continue;
				}
				mesh_rx_have_hdr = true;
			}
			if (mesh_rx_have_hdr && mesh_rx_len >= mesh_rx_expected) {
				mesh_rx_arrive_ms = k_uptime_get_32();
				mesh_rx_proc_len = mesh_rx_expected;
				mesh_rx_processing = true;
				k_work_submit_to_queue(&mesh_rx_wq, &mesh_rx_work);
				break;
			}
		}
	}
}

static int mesh_tx_send(struct ninep_transport *t, const uint8_t *buf, size_t len)
{
	ARG_UNUSED(t);
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(ic_uart, buf[i]);
	}
	return (int)len;
}

static int mesh_tx_start(struct ninep_transport *t)
{
	ARG_UNUSED(t);
	uart_irq_rx_disable(ic_uart);
	uart_irq_callback_user_data_set(ic_uart, mesh_uart_isr, NULL);
	uart_irq_rx_enable(ic_uart);
	return 0;
}

static int mesh_tx_get_mtu(struct ninep_transport *t)
{
	ARG_UNUSED(t);
	return CONFIG_NINEP_MAX_MESSAGE_SIZE;
}

static const struct ninep_transport_ops mesh_tx_ops = {
	.send = mesh_tx_send,
	.start = mesh_tx_start,
	.get_mtu = mesh_tx_get_mtu,
};

/*
 * Reset the inter-chip RX path before a (re)attach. The 9151 can reboot (or
 * glitch uart1 at its own boot) out from under us, leaving stale bytes, a
 * half-parsed frame, or a stuck `mesh_rx_processing` in our ISR accumulator.
 * Any of those silently drops the next reply (the 9151 was GDB-confirmed to send
 * the Rversion, but the client never saw it) and the handshake times out. Drain
 * the UART FIFO and clear the accumulator so the reply parses from a clean state.
 */
static void mesh_rx_reset(void)
{
	/* Clear ONLY the software accumulator state (atomically vs the ISR). Do NOT
	 * touch the UART itself: cycling uart_irq_rx_disable/enable (+ FIFO drain)
	 * here left the nRF53 UARTE RX un-armed, so the very next reply (Rversion)
	 * was never received and the handshake timed out. The ISR stays live; it
	 * just starts accumulating from a clean state. */
	unsigned int key = irq_lock();

	mesh_rx_len = 0;
	mesh_rx_have_hdr = false;
	mesh_rx_expected = 0;
	mesh_rx_processing = false;
	irq_unlock(key);
}

/*
 * Lazily negotiate version + attach to the 9151 root (it may boot later). Takes
 * mesh_sess (brief on a live link -- just snapshots the root; only does the
 * Tversion/Tattach when down, which serializes concurrent re-attaches). On
 * success *root receives the current root fid so the caller walks from a
 * consistent value without holding mesh_sess across the walk.
 */
static int mesh_ensure_attached(uint32_t *root)
{
	int ret;

	k_mutex_lock(&mesh_sess, K_FOREVER);
	if (mesh_attached) {
		if (root) {
			*root = mesh_root_fid;
		}
		k_mutex_unlock(&mesh_sess);
		return 0;
	}
	mesh_rx_reset();  /* clear any boot/reboot garbage before handshaking */
	uint32_t c0 = mesh_rx_isr_calls, b0 = mesh_rx_isr_bytes;
	uint32_t t0 = k_uptime_get_32();

	mesh_rx_arrive_ms = 0;
	ret = ninep_client_version(&mesh_client);
	uint32_t took = k_uptime_get_32() - t0;
	int arrive = mesh_rx_arrive_ms ? (int)(mesh_rx_arrive_ms - t0) : -1;

	LOG_DBG("post-Tversion ret=%d took=%ums; isr_calls=%u isr_bytes=%u Rversion-framed=+%dms",
		ret, took, mesh_rx_isr_calls - c0, mesh_rx_isr_bytes - b0, arrive);
	if (ret < 0) {
		LOG_WRN("9151 Tversion failed: %d", ret);
		k_mutex_unlock(&mesh_sess);
		return ret;
	}
	ret = ninep_client_attach(&mesh_client, &mesh_root_fid, NINEP_NOFID,
				  "relay", "");
	if (ret < 0) {
		LOG_WRN("9151 Tattach failed: %d", ret);
		k_mutex_unlock(&mesh_sess);
		return ret;
	}
	mesh_attached = true;
	mesh_relink_attempts = 0;
	mesh_note_contact();
	if (root) {
		*root = mesh_root_fid;
	}
	LOG_INF("attached to 9151 9P server (root fid %u)", mesh_root_fid);
	k_mutex_unlock(&mesh_sess);
	return 0;
}

/*
 * remote_fs hooks for the /net/aether re-export. root_fn hands the proxy the
 * live 9151 root fid (re-attaching lazily); down_fn drops the session when a
 * proxied walk finds the base fid stale (e.g. the 9151 rebooted), so the next
 * root_fn re-versions+attaches -- the same self-heal the OTA proxy ops use.
 */
static int mesh_remote_root(uint32_t *root, void *user)
{
	ARG_UNUSED(user);
	return mesh_ensure_attached(root);
}

static void mesh_remote_down(void *user)
{
	ARG_UNUSED(user);
	k_mutex_lock(&mesh_sess, K_FOREVER);
	mesh_attached = false;
	k_mutex_unlock(&mesh_sess);
}

/*
 * Background link monitor. Proactively (re)establishes the 9151 session so the
 * link self-heals without a host op having to fail-then-retry first, and so the
 * dev/link9151 status stays fresh. It ONLY probes when the link is down -- a
 * Tversion on a LIVE link would clunk every server fid and break in-flight proxy
 * ops, so an up link is left strictly alone. When the 9151 is offline (e.g. mid
 * OTA swap, the secondary lives in slow external flash so swap-using-move takes
 * minutes and is silent) each probe just times out (~timeout_ms) and bumps the
 * attempt counter; when it returns, the next probe attaches and the link flips
 * back up on its own. Down-transitions are flagged by the proxy ops (walk ->
 * "unknown fid") and by reboot9151, so the monitor never needs to poke a live
 * link to notice it died.
 */
static void mesh_link_monitor_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		bool up, came_up = false;

		k_mutex_lock(&mesh_sess, K_FOREVER);
		up = mesh_attached;
		k_mutex_unlock(&mesh_sess);
		if (!up) {
			if (mesh_ensure_attached(NULL) == 0) {
				mesh_relink_total++;
				up = true;
				came_up = true;
				LOG_INF("link9151: link up (relinks=%u)", mesh_relink_total);
			} else {
				mesh_relink_attempts++;
			}
		}

		/* On a fresh (re)link after an OTA stage -- e.g. the post-swap reboot --
		 * verify the running image and auto-confirm it. The proxy read/confirm
		 * take their own brief locks; the monitor holds none here. */
		if (came_up && fw9151_autoconfirm && fw9151_armed) {
			fw9151_auto_confirm_check();
		}

		/* Probe briskly while down, idle while up. The status read never
		 * takes a mesh lock, so it stays instant even mid-probe. */
		k_msleep(up ? 4000 : 1500);
	}
}

/* dev/link9151 -- read-only health of the 5340<->9151 link. Served entirely from
 * cached 5340 state (no proxy to the possibly-offline 9151), so it answers
 * instantly even while the 9151 is mid-swap and dark. */
static int link9151_read(uint8_t *buf, size_t buf_size, uint64_t off, void *ctx)
{
	ARG_UNUSED(ctx);
	char tmp[160];
	uint32_t last = mesh_last_contact_ms;
	int len;

	if (last == 0) {
		len = snprintf(tmp, sizeof(tmp),
			       "link: %s\nrelink_attempts: %u\nrelinks_total: %u\n"
			       "last_contact: never\n",
			       mesh_attached ? "up" : "down",
			       mesh_relink_attempts, mesh_relink_total);
	} else {
		len = snprintf(tmp, sizeof(tmp),
			       "link: %s\nrelink_attempts: %u\nrelinks_total: %u\n"
			       "last_contact: %us ago\n",
			       mesh_attached ? "up" : "down",
			       mesh_relink_attempts, mesh_relink_total,
			       (k_uptime_get_32() - last) / 1000);
	}
	if (len < 0) {
		return -EIO;
	}
	if (off >= (uint64_t)len) {
		return 0;
	}
	size_t n = MIN(buf_size, (size_t)len - (size_t)off);

	memcpy(buf, tmp + (size_t)off, n);
	return (int)n;
}

static int mesh_client_init(void)
{
	int ret;

	k_mutex_init(&mesh_sess);
	k_mutex_init(&mesh_wr);

	if (!device_is_ready(ic_uart)) {
		LOG_ERR("inter-chip UART (9151 link) not ready");
		return -ENODEV;
	}

	k_work_queue_start(&mesh_rx_wq, mesh_rx_wq_stack,
			   K_THREAD_STACK_SIZEOF(mesh_rx_wq_stack),
			   K_PRIO_PREEMPT(7), NULL);
	k_work_init(&mesh_rx_work, mesh_rx_work_handler);

	mesh_transport.ops = &mesh_tx_ops;
	mesh_transport.priv_data = NULL;

	/* ninep_client_init sets the transport recv_cb + user_data and calls
	 * transport->ops->start (mesh_tx_start) to enable interrupt RX. */
	ret = ninep_client_init(&mesh_client, &mesh_client_cfg, &mesh_transport);
	if (ret < 0) {
		LOG_ERR("mesh client init: %d", ret);
		return ret;
	}
	LOG_INF("9P client up on uart1 -> 9151 (irq RX + workqueue; attach lazy)");

	/* Background link monitor: proactive self-heal + fresh dev/link9151 status. */
	k_thread_create(&mesh_mon_thread, mesh_mon_stack,
			K_THREAD_STACK_SIZEOF(mesh_mon_stack),
			mesh_link_monitor_fn, NULL, NULL, NULL,
			K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&mesh_mon_thread, "link9151_mon");
	return 0;
}

/* /dev/fw9151 -- proxy of the 9151's /dev/firmware over the mesh client. */
static uint32_t fw9151_wfid;     /* remote fid open for the current write stream */
static bool fw9151_writing;
static uint64_t fw9151_woff;

static int fw9151_open_remote(const char *path, uint8_t mode, uint32_t *out_fid)
{
	uint32_t fid;
	int ret = -EIO;

	/*
	 * Up to two attempts. The 9151 can reboot out from under a
	 * previously-established session, leaving mesh_root_fid pointing at a
	 * fid the (new) server has never heard of -- it answers Twalk with
	 * "unknown fid". A sticky mesh_attached would then wedge /dev/fw9151
	 * forever. So on a walk failure, drop the session and re-attach
	 * (fresh Tversion+Tattach), then retry the walk once.
	 */
	for (int attempt = 0; attempt < 2; attempt++) {
		uint32_t root;

		ret = mesh_ensure_attached(&root);   /* brief mesh_sess; snapshots root */
		if (ret < 0) {
			return ret;
		}
		uint32_t wc0 = mesh_rx_isr_calls, wb0 = mesh_rx_isr_bytes;

		mesh_rx_dbg_len = 0;  /* capture the walk's RX bytes */
		/* walk/open run WITHOUT a coarse lock -- the client is per-tag safe,
		 * so concurrent proxy ops interleave and a blocking read holds nothing. */
		ret = ninep_client_walk(&mesh_client, root, &fid, path);
		LOG_DBG("post-Twalk %s ret=%d; uart1 RX during: isr_calls=%u isr_bytes=%u",
			path, ret, mesh_rx_isr_calls - wc0, mesh_rx_isr_bytes - wb0);
		uint32_t dl = mesh_rx_dbg_len;

		if (dl > sizeof(mesh_rx_dbg)) {
			dl = sizeof(mesh_rx_dbg);
		}
		LOG_HEXDUMP_DBG(mesh_rx_dbg, dl, "walk RX bytes:");
		if (ret < 0) {
			LOG_WRN("fw9151: walk %s failed: %d%s", path, ret,
				attempt == 0 ? " -- stale session, re-attaching"
					     : "");
			/* stale root / link down -> drop the session so the next
			 * mesh_ensure_attached re-versions+attaches. */
			k_mutex_lock(&mesh_sess, K_FOREVER);
			mesh_attached = false;
			k_mutex_unlock(&mesh_sess);
			continue;
		}
		ret = ninep_client_open(&mesh_client, fid, mode);
		if (ret < 0) {
			LOG_WRN("fw9151: open %s (mode 0x%02x) failed: %d", path, mode, ret);
			ninep_client_clunk(&mesh_client, fid);
			return ret;
		}
		LOG_INF("fw9151: remote %s open (mode 0x%02x, fid %u)",
			path, mode, fid);
		*out_fid = fid;
		return 0;
	}
	return ret;
}

/* read: proxy the 9151 firmware status (open/read/clunk per call; status is tiny) */
static int fw9151_read(uint8_t *buf, size_t buf_size, uint64_t off, void *ctx)
{
	ARG_UNUSED(ctx);
	uint32_t fid;
	int ret = fw9151_open_remote("dev/firmware", NINEP_OREAD, &fid);

	if (ret < 0) {
		return ret;
	}
	ret = ninep_client_read(&mesh_client, fid, off, buf, buf_size);
	if (ret < 0) {
		LOG_WRN("fw9151: remote read failed: %d", ret);
	} else {
		mesh_note_contact();
	}
	ninep_client_clunk(&mesh_client, fid);
	return ret;
}

/*
 * dev/aether/*: re-export the 9151's mesh node-state (addr/rank/tree/neighbors/
 * routes/...) read-only so the host (and BLE) can watch the DECT mesh form and
 * route. ctx is the 9151-side path. These are short, non-blocking status reads,
 * so open/read/clunk per call is fine -- the same pattern as /dev/fw9151.
 *
 * Curated (one registration per file) on purpose: this is a STATIC set of state
 * files, distinct from the dynamic /net/aether datagram tree (clone/ctl/data),
 * which is carried transparently by the general remote_fs proxy. They live on
 * the 9151's /dev/aether per the spec; the 5340 re-exports them under the same
 * path (a remote_fs mount at /dev/aether would be shadowed by fw_sysfs owning
 * "/" -> /dev, so a sysfs-level forward composes more cleanly here).
 */
static int aether_state_read(uint8_t *buf, size_t buf_size, uint64_t off, void *ctx)
{
	const char *path = ctx;
	uint32_t fid;
	int ret = fw9151_open_remote(path, NINEP_OREAD, &fid);

	if (ret < 0) {
		return ret;
	}
	ret = ninep_client_read(&mesh_client, fid, off, buf, buf_size);
	if (ret >= 0) {
		mesh_note_contact();
	}
	(void)ninep_client_clunk(&mesh_client, fid);
	return ret;
}

/* dev/aether/chat is the writable party-line; forward a write to the 9151. */
static int aether_state_write(const uint8_t *buf, uint32_t count, uint64_t off, void *ctx)
{
	ARG_UNUSED(off);
	const char *path = ctx;
	uint32_t fid;
	int ret = fw9151_open_remote(path, NINEP_OWRITE, &fid);

	if (ret < 0) {
		return ret;
	}
	ret = ninep_client_write(&mesh_client, fid, 0, buf, count);
	(void)ninep_client_clunk(&mesh_client, fid);
	return ret < 0 ? ret : (int)count;
}

/* write: stream a signed image to the 9151's secondary slot (one remote fid) */
static int fw9151_write(const uint8_t *buf, uint32_t count, uint64_t off, void *ctx)
{
	ARG_UNUSED(ctx); ARG_UNUSED(off);
	int ret;

	k_mutex_lock(&mesh_wr, K_FOREVER);
	if (!fw9151_writing) {
		ret = fw9151_open_remote("dev/firmware", NINEP_OWRITE, &fw9151_wfid);
		if (ret < 0) {
			k_mutex_unlock(&mesh_wr);
			return ret;
		}
		fw9151_writing = true;
		fw9151_woff = 0;
		/* Capture the MCUboot image version from the header at the start of the
		 * stream (magic 0x96f3b83d; ih_ver at offset 20) so the auto-confirm
		 * gate can check the running version against what we staged. */
		fw9151_staged_valid = false;
		if (count >= 28 && buf[0] == 0x3d && buf[1] == 0xb8 &&
		    buf[2] == 0xf3 && buf[3] == 0x96) {
			fw9151_staged_major = buf[20];
			fw9151_staged_minor = buf[21];
			fw9151_staged_rev = (uint16_t)(buf[22] | (buf[23] << 8));
			fw9151_staged_valid = true;
			LOG_INF("fw9151: staging image v%u.%u.%u",
				fw9151_staged_major, fw9151_staged_minor, fw9151_staged_rev);
		}
		LOG_INF("fw9151: DFU stream started");
	}
	ret = ninep_client_write(&mesh_client, fw9151_wfid, fw9151_woff, buf, count);
	if (ret < 0) {
		LOG_ERR("fw9151: remote write failed: %d", ret);
		ninep_client_clunk(&mesh_client, fw9151_wfid);
		fw9151_writing = false;
		k_mutex_unlock(&mesh_wr);
		return ret;
	}
	fw9151_woff += ret;
	k_mutex_unlock(&mesh_wr);
	return ret;
}

/* clunk: close the remote fid -> the 9151's ninep_dfu requests its upgrade */
static int fw9151_clunk(void *ctx)
{
	ARG_UNUSED(ctx);
	int ret = 0;

	k_mutex_lock(&mesh_wr, K_FOREVER);
	if (fw9151_writing) {
		ret = ninep_client_clunk(&mesh_client, fw9151_wfid);
		fw9151_writing = false;
		fw9151_armed = fw9151_staged_valid;  /* confirm this version once it boots */
		LOG_INF("fw9151: DFU stream finalized (%llu bytes); 9151 will upgrade%s",
			fw9151_woff, fw9151_armed ? " (auto-confirm armed)" : "");
	}
	k_mutex_unlock(&mesh_wr);
	return ret;
}

/*
 * One-shot proxied write to a remote 9151 control node (walk+open+write+clunk).
 * Used for dev/reboot and dev/confirm so the whole 9151 DFU cycle -- stage,
 * reboot, confirm -- runs over 9P-over-USB with no shell or J-Link. A write of
 * dev/reboot won't get an Rwrite back (the 9151 resets mid-reply); treat the
 * resulting transport error as success once the request is on the wire.
 */
static int fw9151_ctl_write(const char *path, const uint8_t *data, size_t len,
			    bool expect_reset)
{
	uint32_t fid;
	int ret = fw9151_open_remote(path, NINEP_OWRITE, &fid);  /* brief mesh_sess */

	if (ret < 0) {
		return ret;
	}
	ret = ninep_client_write(&mesh_client, fid, 0, data, len);
	(void)ninep_client_clunk(&mesh_client, fid);
	if (expect_reset) {
		/* The node reboots out from under us; drop the stale session. */
		k_mutex_lock(&mesh_sess, K_FOREVER);
		mesh_attached = false;
		k_mutex_unlock(&mesh_sess);
		if (ret < 0) {
			ret = (int)len;  /* request delivered; reset ate the reply */
		}
	}
	return ret;
}

/* dev/reboot9151: proxy a write to the 9151's dev/reboot (swap+reboot). */
static int fw9151_reboot_write(const uint8_t *buf, uint32_t count, uint64_t off, void *ctx)
{
	ARG_UNUSED(buf); ARG_UNUSED(off); ARG_UNUSED(ctx);
	const uint8_t b = '1';
	int ret = fw9151_ctl_write("dev/reboot", &b, 1, true);

	LOG_INF("fw9151: proxied dev/reboot -> %d (9151 swapping/rebooting)", ret);
	return ret < 0 ? ret : (int)count;
}

/* dev/confirm9151: proxy a write to the 9151's dev/confirm (mark image good). */
static int fw9151_confirm_write(const uint8_t *buf, uint32_t count, uint64_t off, void *ctx)
{
	ARG_UNUSED(buf); ARG_UNUSED(off); ARG_UNUSED(ctx);
	const uint8_t b = '1';
	int ret = fw9151_ctl_write("dev/confirm", &b, 1, false);

	LOG_INF("fw9151: proxied dev/confirm -> %d", ret);
	return ret < 0 ? ret : (int)count;
}

/*
 * Auto-confirm gate. Called by the link monitor on a fresh (re)link while armed
 * (an image was staged). Reads the 9151's DFU status over the just-restored link
 * and, if the RUNNING image is the version we staged AND it is still unconfirmed,
 * confirms it. The successful relink + readable /dev/fw9151 IS the health gate:
 * it proves the new image's 9P/OTA path survived the swap -- the property that
 * matters for recoverability. A new image that boots broken never relinks, so it
 * is never confirmed and still auto-reverts on the next reboot (the safety net).
 * Defaults on; disable via dev/fw9151auto (e.g. to test the revert path).
 */
static void fw9151_auto_confirm_check(void)
{
	uint8_t buf[160];
	int n = fw9151_read(buf, sizeof(buf) - 1, 0, NULL);

	if (n < 0) {
		return;  /* link hiccup -- stay armed, retry on the next relink */
	}
	buf[n] = '\0';

	unsigned int cmaj = 0, cmin = 0, crev = 0;
	char *p = strstr((char *)buf, "current ");

	if (p) {
		(void)sscanf(p + 8, "%u.%u.%u", &cmaj, &cmin, &crev);
	}
	char *q = strstr((char *)buf, "confirmed ");
	bool confirmed = q && (strncmp(q + 10, "yes", 3) == 0);
	bool match = (cmaj == fw9151_staged_major && cmin == fw9151_staged_minor &&
		      crev == fw9151_staged_rev);

	if (!confirmed) {
		if (match) {
			const uint8_t one = '1';
			int r = fw9151_ctl_write("dev/confirm", &one, 1, false);

			LOG_INF("fw9151: AUTO-CONFIRMED 9151 v%u.%u.%u (link healthy, matches staged) -> %d",
				cmaj, cmin, crev, r);
			fw9151_armed = false;
		} else {
			LOG_WRN("fw9151: NOT auto-confirming -- running v%u.%u.%u != staged v%u.%u.%u",
				cmaj, cmin, crev, fw9151_staged_major,
				fw9151_staged_minor, fw9151_staged_rev);
			fw9151_armed = false;
		}
	} else if (match) {
		LOG_INF("fw9151: staged v%u.%u.%u already confirmed", cmaj, cmin, crev);
		fw9151_armed = false;
	}
	/* else: an OLD confirmed image is still running (swap not applied yet) --
	 * stay armed and wait for the post-swap relink. */
}

/* dev/fw9151auto: read shows on/off; write '1'/'0' enables/disables auto-confirm. */
static int fw9151auto_read(uint8_t *buf, size_t buf_size, uint64_t off, void *ctx)
{
	ARG_UNUSED(ctx);
	char tmp[24];
	int len = snprintf(tmp, sizeof(tmp), "auto-confirm: %s\n",
			   fw9151_autoconfirm ? "on" : "off");

	if (len < 0) {
		return -EIO;
	}
	if (off >= (uint64_t)len) {
		return 0;
	}
	size_t nn = MIN(buf_size, (size_t)len - (size_t)off);

	memcpy(buf, tmp + (size_t)off, nn);
	return (int)nn;
}

static int fw9151auto_write(const uint8_t *buf, uint32_t count, uint64_t off, void *ctx)
{
	ARG_UNUSED(off); ARG_UNUSED(ctx);
	if (count >= 1 && (buf[0] == '0' || buf[0] == '1')) {
		fw9151_autoconfirm = (buf[0] == '1');
		LOG_INF("fw9151: auto-confirm %s", fw9151_autoconfirm ? "on" : "off");
	}
	return (int)count;
}

/*
 * The 9151's /net/aether is no longer re-exported as a curated, per-file sysfs
 * proxy. It is now a *dynamic* datagram tree (clone/ctl/data conversations, per
 * doc/NET_AETHER_SPEC.md) carried transparently by remote_fs union-mounted at
 * "/net/aether" -- see fw_9p_init(). Node-state files (addr/rank/tree/...) moved
 * to the 9151's /dev/aether per the spec.
 */

static int fw_9p_init(void)
{
	int err;

	if (!device_is_ready(fw_cdc)) {
		LOG_ERR("fw 9P CDC not ready");
		return -ENODEV;
	}

	err = ninep_sysfs_init(&fw_sysfs, fw_sysfs_entries, ARRAY_SIZE(fw_sysfs_entries));
	if (err) {
		LOG_ERR("fw sysfs init: %d", err);
		return err;
	}
	(void)ninep_sysfs_register_writable_file(&fw_sysfs, "dev/reboot",
						 NULL, fw_write_reboot, NULL);
	(void)ninep_sysfs_register_writable_file(&fw_sysfs, "dev/confirm",
						 NULL, fw_write_confirm, NULL);

	/* dev/fw9151: the 9151's /dev/firmware, re-exported over the mesh client. */
	(void)ninep_sysfs_register_writable_file_ex(&fw_sysfs, "dev/fw9151",
						    fw9151_read, fw9151_write,
						    fw9151_clunk, NULL);

	/* Proxied 9151 DFU control: reboot (swap) and confirm (mark good), so the
	 * full stage->reboot->confirm cycle runs over 9P-over-USB, no J-Link. */
	(void)ninep_sysfs_register_writable_file(&fw_sysfs, "dev/reboot9151",
						 NULL, fw9151_reboot_write, NULL);
	(void)ninep_sysfs_register_writable_file(&fw_sysfs, "dev/confirm9151",
						 NULL, fw9151_confirm_write, NULL);

	/* dev/link9151: read-only 5340<->9151 link health (instant, never proxied). */
	(void)ninep_sysfs_register_file(&fw_sysfs, "dev/link9151",
					link9151_read, NULL);

	/* dev/fw9151auto: toggle post-swap auto-confirm of the 9151 (default on). */
	(void)ninep_sysfs_register_writable_file_ex(&fw_sysfs, "dev/fw9151auto",
						    fw9151auto_read, fw9151auto_write,
						    NULL, NULL);

	/* dev/aether/*: the 9151's mesh node-state, proxied read-only over the
	 * mesh link so the host can watch the DECT mesh form + route (neighbors,
	 * routes, tree, rank, ...); chat is the writable party-line. Mirrors the
	 * 9151's /dev/aether path 1:1. */
	(void)ninep_sysfs_register_dir(&fw_sysfs, "dev/aether");
	(void)ninep_sysfs_register_file(&fw_sysfs, "dev/aether/addr",
					aether_state_read, "dev/aether/addr");
	(void)ninep_sysfs_register_file(&fw_sysfs, "dev/aether/rank",
					aether_state_read, "dev/aether/rank");
	(void)ninep_sysfs_register_file(&fw_sysfs, "dev/aether/parent",
					aether_state_read, "dev/aether/parent");
	(void)ninep_sysfs_register_file(&fw_sysfs, "dev/aether/nodeid",
					aether_state_read, "dev/aether/nodeid");
	(void)ninep_sysfs_register_file(&fw_sysfs, "dev/aether/tree",
					aether_state_read, "dev/aether/tree");
	(void)ninep_sysfs_register_file(&fw_sysfs, "dev/aether/neighbors",
					aether_state_read, "dev/aether/neighbors");
	(void)ninep_sysfs_register_file(&fw_sysfs, "dev/aether/routes",
					aether_state_read, "dev/aether/routes");
	(void)ninep_sysfs_register_writable_file_ex(&fw_sysfs, "dev/aether/chat",
						    aether_state_read, aether_state_write,
						    NULL, "dev/aether/chat");

	struct ninep_dfu_config dfu_cfg = {
		.path = "dev/fw5340",
		.status_cb = fw_dfu_status,
	};
	err = ninep_dfu_init(&fw_dfu, &fw_sysfs, &dfu_cfg);
	if (err) {
		LOG_ERR("fw5340 DFU init: %d", err);
		return err;
	}

	/*
	 * Compose the served namespace: fw_sysfs at "/" (the local /dev tree) +
	 * the 9151's /net/aether datagram tree, proxied live over the mesh client
	 * by remote_fs, mounted at "/net/aether". The proxy keeps a persistent
	 * 1:1 host-fid<->9151-fid mapping so clone/ctl/data conversations carry
	 * through transparently (clone allocates a conversation upstream on walk).
	 */
	err = ninep_remote_fs_init(&aether_rfs, &mesh_client, "net/aether",
				   aether_rnodes, ARRAY_SIZE(aether_rnodes),
				   mesh_remote_root, mesh_remote_down, NULL);
	if (err) {
		LOG_ERR("/net/aether remote_fs init: %d", err);
		return err;
	}
	err = ninep_union_fs_init(&fw_union, fw_union_mounts,
				  ARRAY_SIZE(fw_union_mounts));
	if (err) {
		LOG_ERR("union_fs init: %d", err);
		return err;
	}
	err = ninep_union_fs_mount(&fw_union, "/", ninep_sysfs_get_ops(), &fw_sysfs);
	if (err) {
		LOG_ERR("union mount /: %d", err);
		return err;
	}
	err = ninep_union_fs_mount(&fw_union, "/net/aether",
				   ninep_remote_fs_get_ops(), &aether_rfs);
	if (err) {
		LOG_ERR("union mount /net/aether: %d", err);
		return err;
	}

	struct ninep_session_pool_uart_config pool_cfg = {
		.uart_dev = fw_cdc,
		.max_sessions = 1,
		.rx_buf_size_per_session = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.fs_ops = ninep_union_fs_get_ops(),
		.fs_context = &fw_union,
	};
	err = ninep_session_pool_uart_init(&fw_uart_pool, &pool_cfg);
	if (err) {
		LOG_ERR("fw 9P session pool init: %d", err);
		return err;
	}
	err = ninep_session_pool_uart_start(&fw_uart_pool);
	if (err) {
		LOG_ERR("fw 9P session pool start: %d", err);
		return err;
	}

	if (!boot_is_img_confirmed()) {
		LOG_WRN("5340 image not confirmed - write /dev/confirm or it reverts on reboot");
	}
	LOG_INF("fw 9P session pool up on cdc_acm_uart1 (DTR-gated): "
		"/dev/fw5340, /dev/fw9151, /dev/confirm, /dev/reboot");
	return 0;
}

/*
 * The old uart1<->L2CAP byte-pump lived here. It's gone: uart1 is now owned by
 * the 9P client (mesh_client) above, and host access is the re-export server on
 * cdc_acm_uart1. A BLE L2CAP re-export (session_pool_l2cap on the same union)
 * can be added back later; advertising stays up so the device is discoverable.
 */

/* ---- BLE bring-up ---- */

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};
static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("BLE connect failed (0x%02x)", err);
		return;
	}
	LOG_INF("BLE connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("BLE disconnected (0x%02x), re-advertising", reason);
	bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
					BT_GAP_ADV_FAST_INT_MAX_2, NULL),
			ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
}

BT_CONN_CB_DEFINE(conn_cbs) = {
	.connected = connected,
	.disconnected = disconnected,
};

int main(void)
{
	int err;

	LOG_INF("DECT relay/aggregator: 9P client on uart1 -> 9151, re-export on USB");

	/* uart1 is now the 9P client link to the 9151 (replaces the byte-pump).
	 * Init it FIRST so the UART pins are configured and our TX line idles high
	 * before the 9151 boots -- otherwise the 9151 comes up against a floating
	 * RX line and its UART wedges (the link never handshakes). */
	err = mesh_client_init();
	if (err) {
		LOG_ERR("mesh 9P client init failed: %d", err);
	}

	/* We are the interface MCU: now that uart1 is up and idle, release the 9151
	 * from reset so it boots its 9P server against a clean link. It boots (DECT
	 * modem init, etc.) while we bring up USB/BT below; the client attaches
	 * lazily on the first /dev/fw9151 access, by which point it is ready.
	 *
	 * NB this only works with the external J-Link DISCONNECTED. With the DK
	 * J-Link attached (SW2 either way) it sits on the 9151's SWD/nRESET net and
	 * fights this GPIO drive, so the 9151 never releases -- which masked this
	 * reset for the whole bring-up debug. Standalone, P1.07 is ours alone.
	 *
	 * DIAGNOSTIC BUILD: don't drive the reset -- just probe P1.07's natural
	 * level and let the 9151 POR-boot on its own, to test whether our reset
	 * pulse was the thing breaking it. */
	nrf91_probe();
	(void)nrf91_reset;

	/* Bring up USB + the nRF9151-console <-> CDC ACM bridge + re-export server. */
	err = usb_enable(NULL);
	if (err && err != -EALREADY) {
		LOG_ERR("usb_enable failed: %d", err);
	} else if (!device_is_ready(cons_uart) || !device_is_ready(cdc_uart)) {
		LOG_ERR("console-bridge devices not ready");
	} else {
		console_bridge_init();
		LOG_INF("USB CDC console bridge up (9151 shell -> USB serial)");
		err = fw_9p_init();
		if (err) {
			LOG_ERR("fw 9P server init failed: %d", err);
		}
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		return 0;
	}
	err = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
					      BT_GAP_ADV_FAST_INT_MAX_2, NULL),
			      ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("advertising failed: %d", err);
		return 0;
	}

	/* Re-export the SAME /dev tree over BLE L2CAP CoC (in addition to USB-CDC).
	 * BT is up now, so register the L2CAP server here. Non-fatal: if it fails,
	 * the USB re-export is already live. */
	struct ninep_session_pool_l2cap_config l2cap_cfg = {
		.psm = RELAY_PSM,
		.max_sessions = 1,
		.rx_buf_size_per_session = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.fs_ops = ninep_union_fs_get_ops(),
		.fs_context = &fw_union,
	};
	err = ninep_session_pool_l2cap_init(&fw_l2cap_pool, &l2cap_cfg);
	if (err) {
		LOG_ERR("fw 9P L2CAP pool init: %d (BLE re-export off; USB unaffected)", err);
	} else {
		err = ninep_session_pool_l2cap_start(&fw_l2cap_pool);
		if (err) {
			LOG_ERR("fw 9P L2CAP pool start: %d (BLE re-export off)", err);
		} else {
			LOG_INF("fw 9P L2CAP pool up on PSM 0x%04x (same /dev tree over BLE)",
				RELAY_PSM);
		}
	}

	LOG_INF("relay/aggregator ready: advertising + 9P re-export on USB (cdc_acm_uart1) "
		"AND BLE L2CAP (PSM 0x%04x): /dev/fw5340 local, /dev/fw9151 -> 9151", RELAY_PSM);
	return 0;
}
