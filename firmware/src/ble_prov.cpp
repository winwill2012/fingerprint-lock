/**
 * ESP32-C3 BLE 配网实现（Bluedroid GATT Server）
 */
#include "ble_prov.h"
#include "config.h"
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

static BLEServer *gServer = nullptr;
static BLECharacteristic *gConfigChar = nullptr;
static BLECharacteristic *gStatusChar = nullptr;
static bool gBleConnected = false;
static bool gDeviceConnected = false;
static volatile bool gNeedRestartAdv = false;

// 内存缓存，避免主循环频繁打开 NVS
static bool gWifiCredCached = false;
static bool gWifiCredHas = false;
static String gCachedSsid;
static String gCachedPass;

// 主循环待处理（勿在 BLE 回调里直接 WiFi.begin）
static volatile bool gPendingConnect = false;
static volatile bool gPendingClear = false;
static String gPendingSsid;
static String gPendingPass;
static String gLastStatusJson;
static unsigned long gConnectStartedMs = 0;
static bool gWaitingConnectResult = false;

static void wifiRefreshCache() {
  Preferences p;
  if (!p.begin("wifi", true)) {
    gWifiCredHas = false;
    gCachedSsid = "";
    gCachedPass = "";
    gWifiCredCached = true;
    return;
  }
  gCachedSsid = p.getString("ssid", "");
  gCachedPass = p.getString("pass", "");
  p.end();
  gWifiCredHas = gCachedSsid.length() > 0;
  gWifiCredCached = true;
}

bool wifiHasCredentials() {
  if (!gWifiCredCached) wifiRefreshCache();
  return gWifiCredHas;
}

bool wifiLoadCredentials(String &ssid, String &pass) {
  if (!gWifiCredCached) wifiRefreshCache();
  if (!gWifiCredHas) {
    ssid = "";
    pass = "";
    return false;
  }
  ssid = gCachedSsid;
  pass = gCachedPass;
  return true;
}

void wifiSaveCredentials(const String &ssid, const String &pass) {
  Preferences p;
  p.begin("wifi", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
  gCachedSsid = ssid;
  gCachedPass = pass;
  gWifiCredHas = ssid.length() > 0;
  gWifiCredCached = true;
}

void wifiClearCredentials() {
  Preferences p;
  if (p.begin("wifi", false)) {
    p.clear();
    p.end();
  }
  gCachedSsid = "";
  gCachedPass = "";
  gWifiCredHas = false;
  gWifiCredCached = true;
}

static String buildStatusJson() {
  DynamicJsonDocument doc(384);
  wl_status_t st = WiFi.status();
  if (gWaitingConnectResult && st != WL_CONNECTED) {
    doc["wifi"] = "connecting";
  } else if (st == WL_CONNECTED) {
    doc["wifi"] = "connected";
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
  } else if (gWaitingConnectResult) {
    doc["wifi"] = "connecting";
  } else if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL || st == WL_CONNECTION_LOST) {
    doc["wifi"] = "failed";
  } else if (wifiHasCredentials()) {
    doc["wifi"] = "disconnected";
  } else {
    doc["wifi"] = "idle";
  }

  String ssid, pass;
  if (wifiLoadCredentials(ssid, pass)) {
    doc["ssid"] = ssid;
  } else {
    doc["ssid"] = "";
  }
  doc["fw"] = FW_VERSION;
  doc["ble"] = gBleConnected;

  String out;
  serializeJson(doc, out);
  return out;
}

void bleProvNotifyStatus() {
  if (!gStatusChar) return;
  String json = buildStatusJson();
  gLastStatusJson = json;
  gStatusChar->setValue((uint8_t *)json.c_str(), json.length());
  if (gBleConnected) {
    gStatusChar->notify();
  }
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) override {
    gBleConnected = true;
    gDeviceConnected = true;
    Serial.println("[ble] 手机已连接");
    bleProvNotifyStatus();
  }
  void onDisconnect(BLEServer *pServer) override {
    (void)pServer;
    gBleConnected = false;
    gNeedRestartAdv = true;
    Serial.println("[ble] 手机已断开，将重新广播");
  }
};

class ConfigCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    std::string value = pCharacteristic->getValue();
    if (value.empty()) return;

    String payload;
    payload.reserve(value.size());
    for (size_t i = 0; i < value.size(); i++) payload += (char)value[i];
    Serial.printf("[ble] 收到配置: %s\n", payload.c_str());

    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, payload)) {
      Serial.println("[ble] JSON 解析失败");
      return;
    }

    const char *cmd = doc["cmd"] | "";
    if (strcmp(cmd, "clear") == 0) {
      gPendingClear = true;
      return;
    }
    if (strcmp(cmd, "status") == 0) {
      bleProvNotifyStatus();
      return;
    }

    const char *ssid = doc["ssid"] | "";
    const char *pass = doc["password"] | doc["pass"] | "";
    if (ssid[0] == '\0') {
      Serial.println("[ble] 缺少 ssid");
      return;
    }
    gPendingSsid = String(ssid);
    gPendingPass = String(pass);
    gPendingConnect = true;
  }
};

void bleProvBegin() {
  uint64_t mac = ESP.getEfuseMac();
  char name[24];
  snprintf(name, sizeof(name), "FPLock-%04X", (unsigned)(mac & 0xFFFF));

  BLEDevice::init(name);
  BLEDevice::setMTU(517);

  gServer = BLEDevice::createServer();
  gServer->setCallbacks(new ServerCallbacks());

  BLEService *service = gServer->createService(BLE_SERVICE_UUID);

  gConfigChar = service->createCharacteristic(
      BLE_CHAR_CONFIG_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  gConfigChar->setCallbacks(new ConfigCallbacks());

  gStatusChar = service->createCharacteristic(
      BLE_CHAR_STATUS_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  gStatusChar->addDescriptor(new BLE2902());
  gStatusChar->setValue("{}");

  service->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMinPreferred(0x12);

  // 广播包放设备名，扫描响应再带服务 UUID，提高手机可见性
  BLEAdvertisementData advData;
  advData.setFlags(0x06); // LE General Discoverable + BR/EDR Not Supported
  advData.setName(name);
  adv->setAdvertisementData(advData);

  BLEAdvertisementData scanData;
  scanData.setCompleteServices(BLEUUID(BLE_SERVICE_UUID));
  adv->setScanResponseData(scanData);

  BLEDevice::startAdvertising();

  Serial.printf("[ble] 已启动，设备名 %s，可被手机搜索配网\n", name);
  bleProvNotifyStatus();
}

void bleProvLoop() {
  if (gNeedRestartAdv) {
    gNeedRestartAdv = false;
    delay(150);
    BLEDevice::startAdvertising();
    Serial.println("[ble] 已重新开始广播");
  }

  if (gPendingClear) {
    gPendingClear = false;
    gWaitingConnectResult = false;
    wifiClearCredentials();
    WiFi.disconnect(true, true);
    Serial.println("[ble] 已清除 WiFi 凭据");
    bleProvNotifyStatus();
  }

  if (gPendingConnect) {
    gPendingConnect = false;
    wifiSaveCredentials(gPendingSsid, gPendingPass);
    Serial.printf("[ble] 开始连接 WiFi: %s\n", gPendingSsid.c_str());

    WiFi.disconnect(false, false);
    delay(50);
    WiFi.mode(WIFI_STA);
    // BLE 共存必须保持 modem sleep，禁止 setSleep(false)
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    WiFi.begin(gPendingSsid.c_str(), gPendingPass.c_str());

    gConnectStartedMs = millis();
    gWaitingConnectResult = true;
    bleProvNotifyStatus();
  }

  // 等待连接结果并通知 App
  if (gWaitingConnectResult) {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      gWaitingConnectResult = false;
      Serial.printf("[wifi] 已连接 %s  IP=%s\n",
                    WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      bleProvNotifyStatus();
    } else if (millis() - gConnectStartedMs > WIFI_CONNECT_TIMEOUT_MS) {
      gWaitingConnectResult = false;
      Serial.println("[wifi] 连接超时/失败");
      bleProvNotifyStatus();
    } else if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
      gWaitingConnectResult = false;
      Serial.printf("[wifi] 连接失败 status=%d\n", (int)st);
      bleProvNotifyStatus();
    }
  }

  // 连接状态变化时刷新（节流）
  static wl_status_t lastSt = WL_IDLE_STATUS;
  static unsigned long lastNotify = 0;
  wl_status_t nowSt = WiFi.status();
  if (nowSt != lastSt || (gBleConnected && millis() - lastNotify > 5000)) {
    lastSt = nowSt;
    lastNotify = millis();
    if (gBleConnected) bleProvNotifyStatus();
  }
}
