# Brief: Advertising a 9P/L2CAP service like a Thingy53 aetherd node

The goal is to make a 9P↔Aether gateway look, on the BLE side, like an aetherd L2CAP server (the same way `samples/aetherd_server_l2cap` does on Thingy53/nRF5340). Any existing aetherd client — including the iOS scanner and the `l2cap_client` transport in 9p4z — will then discover and connect to your gateway with zero changes.

There are four things to get right: **advertising payload**, **scan response (device name)**, **9PIS GATT service**, and **L2CAP PSM**. All four are required for full client compatibility.

## Reference files

If you're working in a Zephyr/NCS tree that has 9p4z as a module, mirror these:

- `samples/aetherd_server_l2cap/src/main.c` — full reference (advertising, 9PIS init, identity rotation, restart-on-disconnect)
- `samples/aetherd_server_l2cap/prj.conf` — the Kconfig you need
- `9p4z/src/gatt_9pis.c` and `9p4z/include/zephyr/9p/gatt_9pis.h` — the GATT service implementation; just call `ninep_9pis_init(&cfg)` after `bt_enable()`
- `9p4z/docs/9PIS_GATT_SPECIFICATION.md` — wire spec if reimplementing

## 1. Required Kconfig

```
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_EXT_ADV=y                       # iOS requires extended advertising
CONFIG_BT_DEVICE_NAME="AetherBBS"         # any name; clients filter on UUID

CONFIG_NINEP_TRANSPORT_L2CAP=y
CONFIG_NINEP_GATT_9PIS=y
CONFIG_NINEP_L2CAP_PSM=0x0080             # MUST be 0x0080 — clients hardcode/expect this
```

PSM `0x0080` is the Bluetooth-SIG-compliant value aetherd uses everywhere; the old `0x0009` is dead.

## 2. Advertising data (AD)

Two fields, no more:

```c
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    /* 9PIS service UUID — this is what iOS scanners filter on */
    BT_DATA_BYTES(BT_DATA_UUID128_ALL,
        0x01, 0xc0, 0xe4, 0xf6, 0xe0, 0xa1, 0x88, 0xba,
        0x91, 0x4a, 0xed, 0xfe, 0x01, 0x00, 0x50, 0x39),
    /* That's UUID 39500001-feed-4a91-ba88-a1e0f6e4c001, little-endian */
};
```

## 3. Scan response (device name)

```c
static struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, "AetherGW", 8),
};
```

The name is informational only — clients connect by 9PIS UUID + read PSM from the GATT chars. Pick whatever makes sense for the gateway.

## 4. 9PIS GATT service

`CONFIG_NINEP_GATT_9PIS=y` gives you the service for free. After `bt_enable()`, before `bt_le_adv_start()`:

```c
struct ninep_9pis_config gatt_config = {
    .service_description = "Aether Gateway - 9P bridge",
    .service_features    = "bbs,chat,rooms,users",       /* same as aetherd */
    .transport_info      = "l2cap:psm=128,mtu=4096,dynamic,sessions=4",
    .app_store_link      = "https://github.com/jrsharp/aetherbbs",
    .protocol_version    = "9P2000;aetherd;1.0.0",
};
ninep_9pis_init(&gatt_config);
```

**Critical**: `transport_info` must say `psm=128` (decimal of `0x0080`) and `mtu=4096`. The iOS `l2cap_client` reads this characteristic to discover the PSM — it will refuse to connect if this doesn't match.

The 9PIS service UUID and its 5 characteristic UUIDs are:

```
Service:            39500001-feed-4a91-ba88-a1e0f6e4c001
Service Description 39500002-feed-4a91-ba88-a1e0f6e4c001
Features            39500003-feed-4a91-ba88-a1e0f6e4c001
Transport Info      39500004-feed-4a91-ba88-a1e0f6e4c001
App Store Link      39500005-feed-4a91-ba88-a1e0f6e4c001
Protocol Version    39500006-feed-4a91-ba88-a1e0f6e4c001
```

All five chars are **read-only, no auth, no pairing** required.

## 5. L2CAP server

Standard 9p4z session pool on PSM `0x0080`. Cribbed from the sample:

```c
struct ninep_session_pool_l2cap_config cfg = {
    .psm = CONFIG_NINEP_L2CAP_PSM,        /* 0x0080 */
    .fs_ops = your_gateway_fs_ops,
    .fs_ctx = your_gateway_ctx,
    /* ... */
};
ninep_session_pool_l2cap_init(&pool, &cfg);
```

The pool handles `bt_l2cap_server_register` and per-connection session spawning. Size to `CONFIG_BT_MAX_CONN`.

## 6. Multi-connection note (optional but matches Thingy53 behavior)

Thingy53 aetherd pre-creates `CONFIG_BT_ID_MAX` BLE identities and rotates them so multiple clients can each get a dedicated peripheral address. Lift `create_bt_identities()` and the restart-on-disconnect work item verbatim if you want the same behavior. Single-connection gateways can skip this — just `bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd))` once and you're done.

## Verification

- iOS scanner app: device should appear, show service description + features + transport info, and the "Connect" path should reach 9P attach.
- macOS plan9port via a BLE-to-9P bridge: `lsof` on PSM 128.
- Bare `bluetoothctl scan on` should show the device with UUID `39500001-feed-4a91-ba88-a1e0f6e4c001` in the advertised services.

If the iOS client sees the device but won't connect, 95% of the time it's because `transport_info` doesn't say `psm=128` or PSM 0x0080 isn't actually listening yet when advertising starts.
