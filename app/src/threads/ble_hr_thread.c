/*
 * BLE heart-rate peripheral thread.
 *
 * Lifted from zephyr/samples/bluetooth/peripheral_hr: connectable
 * advertising, HRS/BAS/DIS, and re-advertise after disconnect.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/services/hrs.h>
#include <zephyr/logging/log.h>

#include "../shared/shared.h"

LOG_MODULE_REGISTER(ble_hr, LOG_LEVEL_INF);

static bool hrf_ntf_enabled;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_HRS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_BAS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_DIS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

enum {
	STATE_CONNECTED,
	STATE_DISCONNECTED,

	STATE_BITS,
};

static ATOMIC_DEFINE(state, STATE_BITS);

static void connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);

	if (err) {
		LOG_ERR("Connection failed, err 0x%02x %s", err,
			bt_hci_err_to_str(err));
	} else {
		LOG_INF("Connected");
		(void)atomic_set_bit(state, STATE_CONNECTED);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	LOG_INF("Disconnected, reason 0x%02x %s", reason,
		bt_hci_err_to_str(reason));
	(void)atomic_set_bit(state, STATE_DISCONNECTED);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

static void hrs_ntf_changed(bool enabled)
{
	hrf_ntf_enabled = enabled;
	LOG_INF("HRS notification status changed: %s",
		enabled ? "enabled" : "disabled");
}

static struct bt_hrs_cb hrs_cb = {
	.ntf_changed = hrs_ntf_changed,
};

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Pairing cancelled: %s", addr);
}

static struct bt_conn_auth_cb auth_cb_display = {
	.cancel = auth_cancel,
};

static void bas_notify(void)
{
	uint8_t battery_level = bt_bas_get_battery_level();

	battery_level--;
	if (!battery_level) {
		battery_level = 100U;
	}

	bt_bas_set_battery_level(battery_level);
}

static void hrs_notify(void)
{
	static uint8_t heartrate = 90U;

	heartrate++;
	if (heartrate == 160U) {
		heartrate = 90U;
	}

	if (hrf_ntf_enabled) {
		bt_hrs_notify(heartrate);
	}
}

static int start_advertising(void)
{
	int err;

	LOG_INF("Starting Legacy Advertising (connectable and scannable)");
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd,
			      ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return err;
	}

	LOG_INF("Advertising successfully started");
	return 0;
}

void ble_hr_thread(void)
{
	int err;

	k_sem_take(&graphics_ready_sem, K_FOREVER);

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return;
	}

	LOG_INF("Bluetooth initialized");

	bt_conn_auth_cb_register(&auth_cb_display);
	bt_hrs_cb_register(&hrs_cb);

	err = start_advertising();
	if (err) {
		return;
	}

	while (1) {
		k_sleep(K_SECONDS(1));

		hrs_notify();
		bas_notify();

		if (atomic_test_and_clear_bit(state, STATE_CONNECTED)) {
			/* Connected; stop re-advertise until disconnect. */
		} else if (atomic_test_and_clear_bit(state, STATE_DISCONNECTED)) {
			err = start_advertising();
			if (err) {
				return;
			}
		}
	}
}
