/*
 * RGB LED mesh-state indicator. Drives the Thingy:91 X nRF9151 RGB LED
 * (red_led/green_led/blue_led gpio-leds) straight from the HONR mesh state, so
 * convergence is readable across the room with no 9P client:
 *
 *   blue          = HONR root (0x0000)
 *   green         = joined child
 *   amber blink   = unjoined / electing / orphaned
 *   white flash   = a datagram arrived (chat / /net/aether)
 *
 * A periodic work-handler polls g_mesh_ctx (no hooks needed in the HONR state
 * machine), and a mesh recv callback (one of several, since the multi-callback
 * fix) lights the activity flash.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/net/honr.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aether_led, LOG_LEVEL_INF);

/* The mesh context, owned by aether_mesh.c (same handle main.c uses). */
extern struct aether_mesh_ctx *g_mesh_ctx;

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_NODELABEL(red_led), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_NODELABEL(green_led), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led), gpios);

#define LED_TICK_MS 400   /* poll/blink period; also the RX-flash hold time */

static struct k_work_delayable led_work;
static bool blink_phase;
static int64_t last_rx_ms;   /* set by led_rx_cb on each received datagram */

static void rgb(int r, int g, int b)
{
	gpio_pin_set_dt(&led_r, r);
	gpio_pin_set_dt(&led_g, g);
	gpio_pin_set_dt(&led_b, b);
}

/* Mesh recv callback: note the time so the next tick shows the white flash. */
static void led_rx_cb(struct net_if *iface, const uint8_t src[6],
		      const uint8_t *data, size_t len, void *user)
{
	ARG_UNUSED(iface); ARG_UNUSED(src); ARG_UNUSED(data);
	ARG_UNUSED(len); ARG_UNUSED(user);
	last_rx_ms = k_uptime_get();
}

static void led_tick(struct k_work *work)
{
	ARG_UNUSED(work);
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	int r = 0, g = 0, b = 0;

	blink_phase = !blink_phase;

	if (!ctx || !ctx->honr_joined) {
		/* unjoined / electing / orphaned: amber (red+green) blink */
		if (blink_phase) {
			r = 1;
			g = 1;
		}
	} else if (honr_is_root(ctx->honr_addr)) {
		b = 1;   /* root */
	} else {
		g = 1;   /* joined child */
	}

	/* A recent datagram overrides the base colour with a white flash. */
	if (last_rx_ms && (k_uptime_get() - last_rx_ms) < LED_TICK_MS) {
		r = g = b = 1;
	}

	rgb(r, g, b);
	k_work_reschedule(&led_work, K_MSEC(LED_TICK_MS));
}

int aether_led_init(struct net_if *iface)
{
	if (!gpio_is_ready_dt(&led_r) || !gpio_is_ready_dt(&led_g) ||
	    !gpio_is_ready_dt(&led_b)) {
		LOG_ERR("RGB LED GPIOs not ready");
		return -ENODEV;
	}

	gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);

	if (iface) {
		(void)aether_mesh_register_recv_callback(iface, led_rx_cb, NULL);
	}

	k_work_init_delayable(&led_work, led_tick);
	k_work_reschedule(&led_work, K_MSEC(LED_TICK_MS));

	LOG_INF("RGB mesh-state LED up (blue=root, green=child, amber=unjoined, white=rx)");
	return 0;
}
