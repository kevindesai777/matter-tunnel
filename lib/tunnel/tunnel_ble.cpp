/*
 * BLE transport adapter for the VendorTunnel payload protocol.
 *
 * This exists to measure the transport floor. Condition 1 carries the envelope
 * through a Matter vendor cluster over Thread and a border router; this carries
 * *the same envelope bytes*, produced and consumed by *the same* core
 * (tunnel::ProcessRequest), over a direct BLE link with no Matter, no Thread and
 * no hub in the path. The difference between the two is therefore attributable
 * to transport rather than to two similar-looking implementations.
 *
 * 🔴 Deviation to state in the paper: the device still runs the full Matter
 * stack while this is active. This isolates *transport*, not "a device with no
 * Matter" — the Matter stack's memory and scheduling costs are present in both
 * conditions. Nor is the BLE link encrypted (no BT_SMP pairing): Matter's path
 * additionally carries CASE session crypto per message, so this is a floor, not
 * a like-for-like secure-channel comparison.
 *
 * Uses bt_nus directly rather than the door lock's DoorLock::NUSService wrapper:
 * that wrapper's command callback is `void(*)(void *context)` and never sees the
 * received bytes, and it forces BT_NUS_AUTHEN/BT_SMP. Going straight to bt_nus
 * also keeps the upstream application unmodified, and keeps both transport
 * adapters inside this drop-in component.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tunnel_core.h"

#include <platform/CHIPDeviceLayer.h>
#include <platform/Zephyr/BLEAdvertisingArbiter.h>

#include <bluetooth/services/nus.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <array>
#include <cstring>

LOG_MODULE_REGISTER(tunnel_ble, LOG_LEVEL_INF);

namespace {

namespace BleArbiter = chip::DeviceLayer::BLEAdvertisingArbiter;

constexpr std::array<uint8_t, BT_UUID_SIZE_128> kNusUuid{ BT_UUID_NUS_VAL };
constexpr uint8_t kAdvFlags{ BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR };

/* Lower priority than Matter's own commissioning advertising, so this never
 * competes with commissioning. Once commissioned, Matter stops advertising and
 * this takes the slot. */
constexpr uint8_t kAdvPriority = 3;
constexpr uint16_t kAdvIntervalMin = 160; /* 100 ms */
constexpr uint16_t kAdvIntervalMax = 240; /* 150 ms */

BleArbiter::Request sAdvRequest{};
std::array<bt_data, 2> sAdvData;
std::array<bt_data, 1> sScanRsp;

/* Requests are small (a signed request is 79 B) and arrive in a single ATT
 * write at the negotiated MTU, so no reassembly is implemented. */
uint8_t sRxBuf[tunnel::kMaxResponse];
size_t sRxLen;
uint8_t sTxBuf[tunnel::kMaxResponse];

/*
 * 🔴 A DEDICATED work queue, not the system one.
 *
 * ProcessRequest() may build the identity bundle (a DAC signature over X.509)
 * and produce an Ed25519 signature. On the system workqueue - whose stack
 * defaults to ~1 KB - that overflows and takes the device down hard:
 *
 *     ***** MPU FAULT ***** Data Access Violation
 *     ZEPHYR FATAL ERROR 2: Stack overflow  Current thread: sysworkq
 *
 * Observed on hardware: two unsigned echoes succeed, the first SIGNED request
 * halts the device (CONFIG_RESET_ON_FATAL_ERROR=n), and the client only sees
 * the BLE link drop. The Matter transport never hits this because CHIP runs the
 * same core on its own, much larger, stack.
 *
 * Preemptible priority so the Bluetooth RX thread (cooperative) always wins.
 */
constexpr size_t kTunnelStackSize = 8192;
K_THREAD_STACK_DEFINE(sTunnelStack, kTunnelStackSize);
k_work_q sTunnelWq;

k_work sWork;
k_work_delayable sInitWork;
k_work sReadvertiseWork;
k_work_delayable sParamWork;
bt_conn *sConn;
uint8_t sParamRetries;
bt_gatt_exchange_params sMtuParams;

constexpr bt_le_conn_param kFastParams = {
	.interval_min = 6,  /* 7.5 ms */
	.interval_max = 12, /* 15 ms  */
	.latency = 0,
	.timeout = 400,
};

void ProcessWork(k_work *)
{
	tunnel::Reject reject = tunnel::Reject::None;
	const size_t n = tunnel::ProcessRequest(sRxBuf, sRxLen, sTxBuf, sizeof(sTxBuf), &reject);

	if (n == 0) {
		/* No payload response. BLE has no status channel here; the client
		 * observes a timeout. Logged so it is never silent on the device. */
		LOG_WRN("tunnel/ble: rejected (%u)", static_cast<unsigned>(reject));
		return;
	}

	const int err = bt_nus_send(nullptr, sTxBuf, n);
	if (err) {
		/* Most likely the response exceeded ATT_MTU-3. Identity chunks
		 * (405 B) do that at the default MTU; the measured envelopes
		 * (echo and signed request/response) do not. */
		LOG_ERR("tunnel/ble: send failed (%d) for %u B", err, static_cast<unsigned>(n));
	}
}

/*
 * Two things have to happen around the connection itself, and neither is
 * optional for this measurement.
 *
 * 1. MTU. The default ATT MTU of 23 caps a notification at 20 bytes, but a
 *    signed response is ~79. BlueZ did not initiate an MTU exchange, so the
 *    device does it: either peer may start it, and without it every signed
 *    sample times out.
 *
 * 2. Re-advertising. Connectable advertising stops when a central connects and
 *    does not resume by itself, so after the first client disconnected the
 *    device became invisible and the next run could not connect at all without
 *    a power cycle. Re-inserting the arbiter request on disconnect makes runs
 *    repeatable.
 */
void MtuExchanged(bt_conn *conn, uint8_t err, bt_gatt_exchange_params *)
{
	LOG_INF("tunnel/ble: MTU exchange %s, ATT MTU now %u", err ? "FAILED" : "ok",
		bt_gatt_get_mtu(conn));
}

void ReadvertiseHandler(k_work *)
{
	chip::DeviceLayer::PlatformMgr().LockChipStack();
	BleArbiter::CancelRequest(sAdvRequest);
	const CHIP_ERROR err = BleArbiter::InsertRequest(sAdvRequest);
	chip::DeviceLayer::PlatformMgr().UnlockChipStack();
	LOG_INF("tunnel/ble: re-advertise %s", err == CHIP_NO_ERROR ? "requested" : "FAILED");
}

void OnConnected(bt_conn *conn, uint8_t err)
{
	if (err) {
		return;
	}
	sMtuParams.func = MtuExchanged;
	const int rc = bt_gatt_exchange_mtu(conn, &sMtuParams);
	if (rc) {
		LOG_WRN("tunnel/ble: MTU exchange request failed (%d)", rc);
	}

	/*
	 * 🔴 Drive the connection interval to the 7.5 ms minimum.
	 *
	 * Round-trip latency over BLE is quantised by the connection interval: a
	 * request is served at the next connection event and the response at a
	 * later one. BlueZ defaults to ~45 ms, which put the floor at ~90 ms and
	 * made a ~40 ms signing cost COMPLETELY INVISIBLE (measured delta +1.6 ms)
	 * because the signing fitted inside one interval. Measuring the transport
	 * floor at the default interval measures BlueZ's scheduler, not transport.
	 *
	 * Units are 1.25 ms: 6 = 7.5 ms, 12 = 15 ms. Latency 0, supervision 4 s.
	 */
	sConn = conn;
	sParamRetries = 0;
	const int prc = bt_conn_le_param_update(conn, &kFastParams);
	if (prc) {
		LOG_WRN("tunnel/ble: conn param update failed (%d)", prc);
	}
}

/*
 * CHIP's BLE manager re-applies CHIPoBLE connection parameters (45 ms,
 * supervision 500) to this link shortly after it comes up, overriding the fast
 * interval requested at connect. Re-assert it once things have settled; a
 * bounded number of retries, so this can never turn into a renegotiation loop
 * with the stack.
 */
void ParamWorkHandler(k_work *)
{
	if (sConn == nullptr || sParamRetries >= 3) {
		return;
	}
	sParamRetries++;
	const int rc = bt_conn_le_param_update(sConn, &kFastParams);
	LOG_INF("tunnel/ble: re-asserting fast interval (attempt %u, rc=%d)", sParamRetries, rc);
}

void OnParamUpdated(bt_conn *, uint16_t interval, uint16_t latency, uint16_t timeout)
{
	LOG_INF("tunnel/ble: conn params now interval=%u (%u.%02u ms) latency=%u timeout=%u",
		interval, (interval * 125) / 100, (interval * 125) % 100, latency, timeout);

	if (interval > kFastParams.interval_max && sParamRetries < 3) {
		k_work_schedule_for_queue(&sTunnelWq, &sParamWork, K_MSEC(1500));
	}
}

void OnDisconnected(bt_conn *, uint8_t reason)
{
	sConn = nullptr;
	LOG_INF("tunnel/ble: disconnected (reason 0x%02x)", reason);
	k_work_submit_to_queue(&sTunnelWq, &sReadvertiseWork);
}

BT_CONN_CB_DEFINE(tunnel_ble_conn_cbs) = {
	.connected = OnConnected,
	.disconnected = OnDisconnected,
	.le_param_updated = OnParamUpdated,
};

void RxCallback(bt_conn *, const uint8_t *const data, uint16_t len)
{
	if (len == 0 || len > sizeof(sRxBuf)) {
		LOG_WRN("tunnel/ble: bad length %u", len);
		return;
	}

	/* Copy and defer: ProcessRequest may spend ~17 ms in Ed25519 signing,
	 * which must not run on the Bluetooth RX thread. */
	memcpy(sRxBuf, data, len);
	sRxLen = len;
	k_work_submit_to_queue(&sTunnelWq, &sWork);
}

bt_nus_cb sNusCallbacks = {
	.received = RxCallback,
	.sent = nullptr,
	.send_enabled = nullptr,
};


int DoInit()
{
	int err = bt_nus_init(&sNusCallbacks);
	if (err) {
		LOG_ERR("tunnel/ble: bt_nus_init failed (%d)", err);
		return err;
	}

	k_work_queue_init(&sTunnelWq);
	k_work_queue_start(&sTunnelWq, sTunnelStack, K_THREAD_STACK_SIZEOF(sTunnelStack),
			   K_PRIO_PREEMPT(4), nullptr);
	k_thread_name_set(&sTunnelWq.thread, "tunnel_ble");

	k_work_init_delayable(&sParamWork, ParamWorkHandler);
	k_work_init(&sWork, ProcessWork);
	k_work_init(&sReadvertiseWork, ReadvertiseHandler);

	sAdvData[0] = BT_DATA(BT_DATA_FLAGS, &kAdvFlags, sizeof(kAdvFlags));
	sAdvData[1] = BT_DATA(BT_DATA_UUID128_ALL, kNusUuid.data(), kNusUuid.size());

	const char *name = bt_get_name();
	sScanRsp[0] = BT_DATA(BT_DATA_NAME_COMPLETE, name, static_cast<uint8_t>(strlen(name)));

	sAdvRequest = BleArbiter::Request{
		.priority = kAdvPriority,
		.options = BT_LE_ADV_OPT_CONN,
		.minInterval = kAdvIntervalMin,
		.maxInterval = kAdvIntervalMax,
		.advertisingData = chip::Span<bt_data>(sAdvData),
		.scanResponseData = chip::Span<bt_data>(sScanRsp),
		.onStarted = [](int rc) { LOG_INF("tunnel/ble: advertising started (%d)", rc); },
		.onStopped = []() { LOG_INF("tunnel/ble: advertising stopped"); },
	};

	chip::DeviceLayer::PlatformMgr().LockChipStack();
	const CHIP_ERROR chipErr = BleArbiter::InsertRequest(sAdvRequest);
	chip::DeviceLayer::PlatformMgr().UnlockChipStack();

	if (chipErr != CHIP_NO_ERROR) {
		LOG_ERR("tunnel/ble: advertising request rejected");
		return -EIO;
	}

	LOG_INF("tunnel/ble: transport ready as '%s'", name);
	return 0;
}

void InitHandler(k_work *)
{
	DoInit();
}

} // namespace

namespace tunnel {

/*
 * Deferred by design. This is called from the cluster's plugin-init callback,
 * which runs during Matter server initialisation — before the BLE advertising
 * arbiter is necessarily ready to take a request. A short delay is far more
 * robust than guessing the init ordering, and costs nothing: the measurement
 * client connects seconds later at the earliest.
 */
void BleTransportScheduleInit()
{
	k_work_init_delayable(&sInitWork, InitHandler);
	k_work_schedule(&sInitWork, K_SECONDS(5));
}

} // namespace tunnel
