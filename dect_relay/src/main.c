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
#include <zephyr/9p/sysfs.h>
#include <zephyr/9p/dfu.h>
#include <zephyr/9p/client.h>
#include <zephyr/9p/transport_uart.h>
#include <zephyr/9p/protocol.h>
#include <string.h>

LOG_MODULE_REGISTER(dect_relay, LOG_LEVEL_INF);

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
static struct ninep_sysfs fw_sysfs;
static struct ninep_sysfs_entry fw_sysfs_entries[12];
static struct ninep_dfu fw_dfu;

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
	.timeout_ms = 5000,
};
static uint32_t mesh_root_fid;
static bool mesh_attached;
static struct k_mutex mesh_lock;  /* serialize proxy access to the one client */

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
	while (uart_irq_rx_ready(dev)) {
		uint8_t b[64];
		int n = uart_fifo_read(dev, b, sizeof(b));

		if (n <= 0) {
			break;
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

/* Lazily negotiate version + attach to the 9151 root (it may boot later). */
static int mesh_ensure_attached(void)
{
	int ret;

	if (mesh_attached) {
		return 0;
	}
	ret = ninep_client_version(&mesh_client);
	if (ret < 0) {
		LOG_WRN("9151 Tversion failed: %d", ret);
		return ret;
	}
	ret = ninep_client_attach(&mesh_client, &mesh_root_fid, NINEP_NOFID,
				  "relay", "");
	if (ret < 0) {
		LOG_WRN("9151 Tattach failed: %d", ret);
		return ret;
	}
	mesh_attached = true;
	LOG_INF("attached to 9151 9P server (root fid %u)", mesh_root_fid);
	return 0;
}

static int mesh_client_init(void)
{
	int ret;

	k_mutex_init(&mesh_lock);

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
	return 0;
}

/* /dev/fw9151 -- proxy of the 9151's /dev/firmware over the mesh client. */
static uint32_t fw9151_wfid;     /* remote fid open for the current write stream */
static bool fw9151_writing;
static uint64_t fw9151_woff;

static int fw9151_open_remote(uint8_t mode, uint32_t *out_fid)
{
	uint32_t fid;
	int ret = mesh_ensure_attached();

	if (ret < 0) {
		return ret;
	}
	ret = ninep_client_walk(&mesh_client, mesh_root_fid, &fid, "dev/firmware");
	if (ret < 0) {
		LOG_WRN("fw9151: walk dev/firmware failed: %d", ret);
		return ret;
	}
	ret = ninep_client_open(&mesh_client, fid, mode);
	if (ret < 0) {
		LOG_WRN("fw9151: open (mode 0x%02x) failed: %d", mode, ret);
		ninep_client_clunk(&mesh_client, fid);
		return ret;
	}
	LOG_INF("fw9151: remote dev/firmware open (mode 0x%02x, fid %u)", mode, fid);
	*out_fid = fid;
	return 0;
}

/* read: proxy the 9151 firmware status (open/read/clunk per call; status is tiny) */
static int fw9151_read(uint8_t *buf, size_t buf_size, uint64_t off, void *ctx)
{
	ARG_UNUSED(ctx);
	uint32_t fid;
	int ret;

	k_mutex_lock(&mesh_lock, K_FOREVER);
	ret = fw9151_open_remote(NINEP_OREAD, &fid);
	if (ret < 0) {
		k_mutex_unlock(&mesh_lock);
		return ret;
	}
	ret = ninep_client_read(&mesh_client, fid, off, buf, buf_size);
	if (ret < 0) {
		LOG_WRN("fw9151: remote read failed: %d", ret);
	}
	ninep_client_clunk(&mesh_client, fid);
	k_mutex_unlock(&mesh_lock);
	return ret;
}

/* write: stream a signed image to the 9151's secondary slot (one remote fid) */
static int fw9151_write(const uint8_t *buf, uint32_t count, uint64_t off, void *ctx)
{
	ARG_UNUSED(ctx); ARG_UNUSED(off);
	int ret;

	k_mutex_lock(&mesh_lock, K_FOREVER);
	if (!fw9151_writing) {
		ret = fw9151_open_remote(NINEP_OWRITE, &fw9151_wfid);
		if (ret < 0) {
			k_mutex_unlock(&mesh_lock);
			return ret;
		}
		fw9151_writing = true;
		fw9151_woff = 0;
		LOG_INF("fw9151: DFU stream started");
	}
	ret = ninep_client_write(&mesh_client, fw9151_wfid, fw9151_woff, buf, count);
	if (ret < 0) {
		LOG_ERR("fw9151: remote write failed: %d", ret);
		ninep_client_clunk(&mesh_client, fw9151_wfid);
		fw9151_writing = false;
		k_mutex_unlock(&mesh_lock);
		return ret;
	}
	fw9151_woff += ret;
	k_mutex_unlock(&mesh_lock);
	return ret;
}

/* clunk: close the remote fid -> the 9151's ninep_dfu requests its upgrade */
static int fw9151_clunk(void *ctx)
{
	ARG_UNUSED(ctx);
	int ret = 0;

	k_mutex_lock(&mesh_lock, K_FOREVER);
	if (fw9151_writing) {
		ret = ninep_client_clunk(&mesh_client, fw9151_wfid);
		fw9151_writing = false;
		LOG_INF("fw9151: DFU stream finalized (%llu bytes); 9151 will upgrade",
			fw9151_woff);
	}
	k_mutex_unlock(&mesh_lock);
	return ret;
}

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

	struct ninep_dfu_config dfu_cfg = {
		.path = "dev/fw5340",
		.status_cb = fw_dfu_status,
	};
	err = ninep_dfu_init(&fw_dfu, &fw_sysfs, &dfu_cfg);
	if (err) {
		LOG_ERR("fw5340 DFU init: %d", err);
		return err;
	}

	struct ninep_session_pool_uart_config pool_cfg = {
		.uart_dev = fw_cdc,
		.max_sessions = 1,
		.rx_buf_size_per_session = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.fs_ops = ninep_sysfs_get_ops(),
		.fs_context = &fw_sysfs,
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

	/* uart1 is now the 9P client link to the 9151 (replaces the byte-pump). */
	err = mesh_client_init();
	if (err) {
		LOG_ERR("mesh 9P client init failed: %d", err);
	}

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

	LOG_INF("relay/aggregator ready: advertising; re-export on cdc_acm_uart1 "
		"(/dev/fw5340 local, /dev/fw9151 -> 9151)");
	return 0;
}
