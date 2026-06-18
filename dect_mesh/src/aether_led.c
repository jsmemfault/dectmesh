/*
 * Mesh-state LED indicator. Drives a board LED set straight from the HONR mesh
 * state so convergence is readable across the room with no 9P client.
 *
 * Two board renderings (selected by devicetree, see below):
 *
 *   Thingy:91 X — RGB LED (red_led/green_led/blue_led):
 *     blue        = HONR root (0x0000)
 *     green       = joined child
 *     amber blink = unjoined / electing / orphaned
 *     white flash = a datagram arrived (chat / /net/aether)
 *
 *   nRF9151 DK — 4 discrete LEDs (topo_alive/child/root/rx, defined in the DK
 *   board overlay; the DK has no RGB LED):
 *     LED1 (alive) = solid when settled in the mesh, blink when searching
 *                    (unjoined / electing / orphaned)
 *     LED2 (child) = solid when joined as a child
 *     LED3 (root)  = solid when this node is the HONR root
 *     LED4 (rx)    = flash when a datagram arrived
 *
 * A periodic work-handler polls g_mesh_ctx (no hooks needed in the HONR state
 * machine), and a mesh recv callback lights the activity flash. The tick/state
 * logic is shared; only the LED hardware + render() differ per board.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/net/honr.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aether_led, LOG_LEVEL_INF);

/* The mesh context, owned by aether_mesh.c (same handle main.c uses). */
extern struct aether_mesh_ctx *g_mesh_ctx;

#define LED_TICK_MS 400   /* poll/blink period; also the RX-flash hold time */

enum mesh_led_state { MLED_UNJOINED, MLED_ROOT, MLED_CHILD };

#if DT_NODE_EXISTS(DT_NODELABEL(topo_root))
/* ---- nRF9151 DK: 4 discrete LEDs, one meaning each ---- */
static const struct gpio_dt_spec led_alive = GPIO_DT_SPEC_GET(DT_NODELABEL(topo_alive), gpios);
static const struct gpio_dt_spec led_child = GPIO_DT_SPEC_GET(DT_NODELABEL(topo_child), gpios);
static const struct gpio_dt_spec led_root  = GPIO_DT_SPEC_GET(DT_NODELABEL(topo_root), gpios);
static const struct gpio_dt_spec led_rx    = GPIO_DT_SPEC_GET(DT_NODELABEL(topo_rx), gpios);

#define LED_SCHEME_DESC "DK 4-LED (LED1=alive, LED2=child, LED3=root, LED4=rx)"

static bool led_hw_ready(void)
{
	return gpio_is_ready_dt(&led_alive) && gpio_is_ready_dt(&led_child) &&
	       gpio_is_ready_dt(&led_root) && gpio_is_ready_dt(&led_rx);
}

static void led_hw_configure(void)
{
	gpio_pin_configure_dt(&led_alive, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_child, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_root, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_rx, GPIO_OUTPUT_INACTIVE);
}

static void led_render(enum mesh_led_state st, bool blink, bool rx)
{
	bool joined = (st != MLED_UNJOINED);

	/* alive: solid once settled in the mesh, blink while searching */
	gpio_pin_set_dt(&led_alive, joined ? 1 : (blink ? 1 : 0));
	gpio_pin_set_dt(&led_child, st == MLED_CHILD ? 1 : 0);
	gpio_pin_set_dt(&led_root, st == MLED_ROOT ? 1 : 0);
	gpio_pin_set_dt(&led_rx, rx ? 1 : 0);
}
#else
/* ---- Thingy:91 X: RGB LED ---- */
static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_NODELABEL(red_led), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_NODELABEL(green_led), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led), gpios);

#define LED_SCHEME_DESC "RGB (blue=root, green=child, amber=unjoined, white=rx)"

static bool led_hw_ready(void)
{
	return gpio_is_ready_dt(&led_r) && gpio_is_ready_dt(&led_g) &&
	       gpio_is_ready_dt(&led_b);
}

static void led_hw_configure(void)
{
	gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);
}

static void led_render(enum mesh_led_state st, bool blink, bool rx)
{
	int r = 0, g = 0, b = 0;

	if (st == MLED_UNJOINED) {
		if (blink) {   /* amber blink */
			r = 1;
			g = 1;
		}
	} else if (st == MLED_ROOT) {
		b = 1;
	} else {
		g = 1;
	}

	if (rx) {   /* white flash overrides the base colour */
		r = g = b = 1;
	}

	gpio_pin_set_dt(&led_r, r);
	gpio_pin_set_dt(&led_g, g);
	gpio_pin_set_dt(&led_b, b);
}
#endif

static struct k_work_delayable led_work;
static bool blink_phase;
static int64_t last_rx_ms;   /* set by led_rx_cb on each received datagram */

/* Mesh recv callback: note the time so the next tick shows the activity flash. */
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
	enum mesh_led_state st;
	bool rx;

	blink_phase = !blink_phase;

	if (!ctx || !ctx->honr_joined) {
		st = MLED_UNJOINED;
	} else if (honr_is_root(ctx->honr_addr)) {
		st = MLED_ROOT;
	} else {
		st = MLED_CHILD;
	}

	rx = (last_rx_ms && (k_uptime_get() - last_rx_ms) < LED_TICK_MS);

	led_render(st, blink_phase, rx);
	k_work_reschedule(&led_work, K_MSEC(LED_TICK_MS));
}

int aether_led_init(struct net_if *iface)
{
	if (!led_hw_ready()) {
		LOG_ERR("mesh-state LED GPIOs not ready");
		return -ENODEV;
	}

	led_hw_configure();

	if (iface) {
		(void)aether_mesh_register_recv_callback(iface, led_rx_cb, NULL);
	}

	k_work_init_delayable(&led_work, led_tick);
	k_work_reschedule(&led_work, K_MSEC(LED_TICK_MS));

	LOG_INF("mesh-state LED up: " LED_SCHEME_DESC);
	return 0;
}
