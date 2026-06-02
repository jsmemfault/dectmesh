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
};
static struct bridge_ep cons_ep; /* self = uart0 (9151 console) */
static struct bridge_ep cdc_ep;  /* self = CDC ACM (host)       */

static void bridge_isr(const struct device *dev, void *user_data)
{
	struct bridge_ep *ep = user_data;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
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
				      .tx_ring = &host_to_cons_rb };
	cdc_ep = (struct bridge_ep){ .self = cdc_uart, .peer = cons_uart,
				     .rx_ring = &host_to_cons_rb,
				     .tx_ring = &cons_to_host_rb };

	uart_irq_callback_user_data_set(cons_uart, bridge_isr, &cons_ep);
	uart_irq_callback_user_data_set(cdc_uart, bridge_isr, &cdc_ep);
	uart_irq_rx_enable(cons_uart);
	uart_irq_rx_enable(cdc_uart);
}

/* L2CAP TX SDU pool. */
NET_BUF_POOL_DEFINE(relay_tx_pool, 4, BT_L2CAP_SDU_BUF_SIZE(RELAY_MTU),
		    CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

/* UART -> L2CAP staging. */
RING_BUF_DECLARE(uart_rx_rb, UART_RING_SIZE);
static K_SEM_DEFINE(uart_rx_sem, 0, 1);

/* The single active relay channel. */
struct relay_chan {
	struct bt_l2cap_le_chan le;
	bool connected;
};
static struct relay_chan relay_chan;
static struct relay_chan *active_chan;

/* ---- UART: ISR drains RX into the ring buffer, TX is polled ---- */

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	uint8_t buf[64];
	int n;

	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		n = uart_fifo_read(dev, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		(void)ring_buf_put(&uart_rx_rb, buf, n);
		k_sem_give(&uart_rx_sem);
	}
}

static void uart_write_all(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(ic_uart, data[i]);
	}
}

/* ---- L2CAP CoC server ---- */

static int l2cap_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	ARG_UNUSED(chan);
	/* L2CAP SDU from the client -> forward verbatim to the 9151 over UART. */
	uart_write_all(buf->data, buf->len);
	return 0; /* NCS grants credits internally */
}

static void l2cap_connected(struct bt_l2cap_chan *chan)
{
	struct bt_l2cap_le_chan *le = BT_L2CAP_LE_CHAN(chan);
	struct relay_chan *rc = CONTAINER_OF(le, struct relay_chan, le);

	rc->connected = true;
	active_chan = rc;
	LOG_INF("L2CAP relay channel up (MTU rx=%u tx=%u)", le->rx.mtu, le->tx.mtu);
}

static void l2cap_disconnected(struct bt_l2cap_chan *chan)
{
	struct bt_l2cap_le_chan *le = BT_L2CAP_LE_CHAN(chan);
	struct relay_chan *rc = CONTAINER_OF(le, struct relay_chan, le);

	rc->connected = false;
	if (active_chan == rc) {
		active_chan = NULL;
	}
	LOG_INF("L2CAP relay channel down");
}

static const struct bt_l2cap_chan_ops chan_ops = {
	.connected = l2cap_connected,
	.disconnected = l2cap_disconnected,
	.recv = l2cap_recv,
};

static int l2cap_accept(struct bt_conn *conn, struct bt_l2cap_server *server,
			struct bt_l2cap_chan **chan)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(server);

	if (relay_chan.connected) {
		return -ENOMEM; /* single client */
	}
	memset(&relay_chan, 0, sizeof(relay_chan));
	relay_chan.le.chan.ops = &chan_ops;
	relay_chan.le.rx.mtu = RELAY_MTU;
	*chan = &relay_chan.le.chan;
	return 0;
}

static struct bt_l2cap_server l2cap_server = {
	.psm = RELAY_PSM,
	.sec_level = BT_SECURITY_L1, /* link is open; auth is at the 9P/factotum layer */
	.accept = l2cap_accept,
};

/* ---- UART -> L2CAP pump thread ---- */

static void relay_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	uint8_t chunk[L2CAP_CHUNK_MAX];

	while (1) {
		k_sem_take(&uart_rx_sem, K_FOREVER);

		uint32_t n;
		while ((n = ring_buf_get(&uart_rx_rb, chunk, sizeof(chunk))) > 0) {
			if (!active_chan || !active_chan->connected) {
				continue; /* no client; drop (9P client will retry) */
			}
			struct net_buf *nb = net_buf_alloc(&relay_tx_pool, K_MSEC(100));
			if (!nb) {
				LOG_WRN("no TX buf, dropping %u bytes", n);
				continue;
			}
			net_buf_reserve(nb, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
			net_buf_add_mem(nb, chunk, n);
			int ret = bt_l2cap_chan_send(&active_chan->le.chan, nb);
			if (ret < 0) {
				LOG_WRN("l2cap send failed: %d", ret);
				net_buf_unref(nb);
			}
		}
	}
}
K_THREAD_DEFINE(relay_tid, 2048, relay_thread, NULL, NULL, NULL, 5, 0, 0);

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

	LOG_INF("DECT mesh BLE relay (L2CAP PSM 0x%04x <-> uart1)", RELAY_PSM);

	if (!device_is_ready(ic_uart)) {
		LOG_ERR("inter-chip UART not ready");
		return 0;
	}
	uart_irq_callback_user_data_set(ic_uart, uart_isr, NULL);
	uart_irq_rx_enable(ic_uart);

	/* Bring up USB + the nRF9151-console <-> CDC ACM bridge. */
	err = usb_enable(NULL);
	if (err && err != -EALREADY) {
		LOG_ERR("usb_enable failed: %d", err);
	} else if (!device_is_ready(cons_uart) || !device_is_ready(cdc_uart)) {
		LOG_ERR("console-bridge devices not ready");
	} else {
		console_bridge_init();
		LOG_INF("USB CDC console bridge up (9151 shell -> USB serial)");
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		return 0;
	}
	err = bt_l2cap_server_register(&l2cap_server);
	if (err) {
		LOG_ERR("l2cap server register failed: %d", err);
		return 0;
	}
	err = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
					      BT_GAP_ADV_FAST_INT_MAX_2, NULL),
			      ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("advertising failed: %d", err);
		return 0;
	}

	LOG_INF("relay ready: advertising, L2CAP server on PSM 0x%04x", RELAY_PSM);
	return 0;
}
