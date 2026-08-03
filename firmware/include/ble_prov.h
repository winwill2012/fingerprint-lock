#pragma once
/**
 * BLE 低功耗蓝牙配网
 * App 通过 GATT 下发 WiFi SSID/密码，设备保存到 NVS 并连接。
 */

#include <Arduino.h>

// 自定义 GATT UUID（与 App 端保持一致）
#define BLE_SERVICE_UUID      "a5a5a501-7f3c-4a1b-9e2d-1234567890ab"
#define BLE_CHAR_CONFIG_UUID  "a5a5a502-7f3c-4a1b-9e2d-1234567890ab"
#define BLE_CHAR_STATUS_UUID  "a5a5a503-7f3c-4a1b-9e2d-1234567890ab"

/** 启动 BLE 广播与 GATT 服务（设备名 FPLock-XXXX） */
void bleProvBegin();

/** 主循环调用：处理待执行的配网/清凭据请求，并刷新状态通知 */
void bleProvLoop();

/** 主动推送当前 WiFi 状态到 STATUS 特征 */
void bleProvNotifyStatus();

/** NVS 是否已有 WiFi 凭据 */
bool wifiHasCredentials();

/** 从 NVS 读取凭据 */
bool wifiLoadCredentials(String &ssid, String &pass);

/** 保存凭据到 NVS */
void wifiSaveCredentials(const String &ssid, const String &pass);

/** 清除 NVS 凭据 */
void wifiClearCredentials();
