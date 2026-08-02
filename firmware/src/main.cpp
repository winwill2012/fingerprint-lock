/**
 * ============================================================
 * 智能指纹锁固件 —— ESP32-C3
 * ------------------------------------------------------------
 * 硬件：
 *   - 指纹模块（海凌科 AS608 / R307，8 字节 UART 协议）
 *      模块 TX -> IO4 (Serial1 RX)
 *      模块 RX <- IO5 (Serial1 TX)
 *   - 电子锁：IO0 高电平开锁，脉冲宽度 LOCK_PULSE_MS（默认 300ms，≤500ms）
 *   - 电池：IO3 (ADC1_CH3) 经两个 100K 分压采样 16340 电池电压
 * 功能：
 *   - 指纹模块工作在校验模式（1:N 搜索比对），识别成功即开锁
 *   - MQTT 远程开锁 / 指纹录入、删除、清空、列表、备注 / 状态与电量上报
 * ============================================================
 */
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_Fingerprint.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>
#include "config.h"

// ---------------- 硬件对象 ----------------
// Serial1 = UART1，用于指纹模块
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial1);
WiFiClient netClient;
PubSubClient mqtt(netClient);
Preferences prefs;

// ---------------- 开锁状态 ----------------
static bool lockActive = false;        // 开锁脉冲进行中
static unsigned long lockStartMs = 0;  // 脉冲开始时间
static unsigned long lastUnlockAt = 0; // 上次开锁时间（冷却用）

// ---------------- 指纹录入状态机 ----------------
static bool enrollBusy = false;
static int enrollId = -1;
static String enrollNote;
static uint8_t enrollStep = 0;         // 0=等第1次按压 1=等待抬手 2=等第2次按压并合成
static unsigned long enrollDeadline = 0;

// ---------------- 指纹注册表（NVS 保存 id/备注/录入时间） ----------------
struct FpEntry {
  int id;
  String note;
  unsigned long ts;
};
static std::vector<FpEntry> fpRegistry;

// ---------------- 其他状态 ----------------
static unsigned long lastBatteryReport = 0;
static unsigned long lastNotFoundPublish = 0;
static unsigned long lastWifiRetry = 0;
static unsigned long lastMqttRetry = 0;
static bool fpReady = false;

// ============================================================
//  工具函数
// ============================================================
static unsigned long nowSec() {
  time_t t = time(nullptr);
  return (t > 1600000000) ? (unsigned long)t : (unsigned long)(millis() / 1000);
}

// ============================================================
//  电池
// ============================================================
int readBatteryMv() {
  long sum = 0;
  for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
    sum += analogReadMilliVolts(PIN_BAT);
    delayMicroseconds(300);
  }
  int mv = (int)((sum / BAT_ADC_SAMPLES) * BAT_DIVIDER);
  return constrain(mv, 0, 5000);
}

int batteryPercent(int mv) {
  // 16340 锂电池放电曲线（近似分段线性）
  static const int pts[][2] = {
    {4200, 100}, {4100, 95}, {4000, 88}, {3900, 78}, {3800, 68}, {3700, 57},
    {3600, 46}, {3500, 35}, {3400, 24}, {3300, 15}, {3200, 8}, {3100, 3}, {3000, 0}
  };
  const int n = sizeof(pts) / sizeof(pts[0]);
  if (mv >= pts[0][0]) return 100;
  if (mv <= pts[n - 1][0]) return 0;
  for (int i = 0; i < n - 1; i++) {
    if (mv <= pts[i][0] && mv >= pts[i + 1][0]) {
      float ratio = (float)(pts[i][0] - mv) / (pts[i][0] - pts[i + 1][0]);
      return pts[i][1] - (int)((pts[i][1] - pts[i + 1][1]) * ratio);
    }
  }
  return 50;
}

// ============================================================
//  MQTT 发布
// ============================================================
void publishDoc(DynamicJsonDocument &doc) {
  if (!mqtt.connected()) return;
  char buf[4096];   // 指纹列表 JSON 可能较大
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(MQTT_RESP_TOPIC, (const uint8_t *)buf, n, false);
}

void publishEvent(const char *type, const char *phase, int id, int extra, const char *note) {
  DynamicJsonDocument doc(512);
  doc["type"] = type;
  if (phase) doc["phase"] = phase;
  if (id >= 0) doc["id"] = id;
  if (extra >= 0) doc["extra"] = extra;
  if (note && strlen(note)) doc["note"] = note;
  publishDoc(doc);
}

void publishError(const char *code) {
  publishEvent("error", code, -1, -1, nullptr);
}

void publishStatus(bool force) {
  (void)force;
  int mv = readBatteryMv();
  DynamicJsonDocument doc(768);
  doc["type"] = "status";
  doc["online"] = true;
  doc["battery_mv"] = mv;
  doc["battery_pct"] = batteryPercent(mv);
  doc["fp_count"] = fpReady ? finger.templateCount : -1;
  doc["fp_reg"] = (int)fpRegistry.size();
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = (int)(millis() / 1000);
  doc["enroll_busy"] = enrollBusy;
  doc["fw"] = FW_VERSION;
  publishDoc(doc);
}

void publishUnlock(const char *source, int fpId, int confidence) {
  int mv = readBatteryMv();
  DynamicJsonDocument doc(512);
  doc["type"] = "unlock";
  doc["source"] = source;
  doc["fp_id"] = fpId;
  doc["confidence"] = confidence;
  doc["battery_mv"] = mv;
  doc["battery_pct"] = batteryPercent(mv);
  doc["ts"] = (long)nowSec();
  publishDoc(doc);
}

// ============================================================
//  开锁控制（脉冲 ≤ LOCK_PULSE_MS，非阻塞）
// ============================================================
void requestUnlock(const char *source, int fpId, int confidence) {
  unsigned long now = millis();
  if (lockActive) return;                                  // 已有脉冲进行中
  if (now - lastUnlockAt < UNLOCK_COOLDOWN_MS) return;     // 冷却防连发
  lastUnlockAt = now;
  lockStartMs = now;
  lockActive = true;
  digitalWrite(PIN_LOCK, HIGH);                            // 开锁
  publishUnlock(source, fpId, confidence);
}

void tickLock() {
  if (lockActive && (millis() - lockStartMs >= LOCK_PULSE_MS)) {
    digitalWrite(PIN_LOCK, LOW);                           // 关锁
    lockActive = false;
    Serial.printf("[lock] 脉冲结束，通电 %.0f ms\n", (double)(millis() - lockStartMs));
  }
}

// ============================================================
//  指纹注册表（NVS）
// ============================================================
void fpLoadRegistry() {
  fpRegistry.clear();
  prefs.begin("fp", false);
  String s = prefs.getString("meta", "");
  if (!s.isEmpty()) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, s)) {
      JsonArray arr = doc.as<JsonArray>();
      for (JsonObject o : arr) {
        FpEntry e;
        e.id = o["id"] | -1;
        e.note = (const char *)(o["note"] | "");
        e.ts = o["ts"] | 0UL;
        if (e.id >= 0) fpRegistry.push_back(e);
      }
    }
  }
  prefs.end();
  Serial.printf("[fp] 注册表已加载，共 %d 条\n", (int)fpRegistry.size());
}

void fpSaveRegistry() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (auto &e : fpRegistry) {
    JsonObject o = arr.createNestedObject();
    o["id"] = e.id;
    o["note"] = e.note;
    o["ts"] = e.ts;
  }
  String out;
  serializeJson(doc, out);
  prefs.begin("fp", false);
  prefs.putString("meta", out);
  prefs.end();
}

void fpAddEntry(int id, const String &note) {
  for (auto &e : fpRegistry) {
    if (e.id == id) { e.note = note; e.ts = nowSec(); fpSaveRegistry(); return; }
  }
  FpEntry e;
  e.id = id; e.note = note; e.ts = nowSec();
  fpRegistry.push_back(e);
  fpSaveRegistry();
}

bool fpRenameEntry(int id, const String &note) {
  for (auto &e : fpRegistry) {
    if (e.id == id) { e.note = note; fpSaveRegistry(); return true; }
  }
  return false;
}

void fpRemoveEntry(int id) {
  for (size_t i = 0; i < fpRegistry.size(); i++) {
    if (fpRegistry[i].id == id) {
      fpRegistry.erase(fpRegistry.begin() + i);
      fpSaveRegistry();
      return;
    }
  }
}

void fpClearRegistry() {
  fpRegistry.clear();
  fpSaveRegistry();
}

void refreshFpCount() {
  if (fpReady) finger.getTemplateCount();
}

// ============================================================
//  指纹模块初始化（带重试，模块上电需要时间）
// ============================================================
bool initFingerprint() {
  for (int i = 0; i < 10; i++) {
    if (finger.verifyPassword()) {
      finger.getTemplateCount();
      Serial.printf("[fp] 指纹模块就绪，已存模板 %d\n", finger.templateCount);
      return true;
    }
    delay(500);
  }
  return false;
}

// ============================================================
//  指纹录入状态机（由主循环驱动）
// ============================================================
void enrollTick() {
  if (millis() > enrollDeadline) {
    enrollBusy = false;
    publishEvent("enroll", "failed", enrollId, -1, "timeout");
    return;
  }

  if (enrollStep == 0) {                    // 等待第 1 次按压
    uint8_t r = finger.getImage();
    if (r == FINGERPRINT_OK) {
      r = finger.image2Tz(1);
      if (r == FINGERPRINT_OK) {
        enrollStep = 1;
        enrollDeadline = millis() + ENROLL_TIMEOUT_MS;
        publishEvent("enroll", "finger1_ok", enrollId, -1, nullptr);
        publishEvent("enroll", "lift_finger", enrollId, -1, nullptr);
      } else {
        enrollBusy = false;
        publishEvent("enroll", "failed", enrollId, r, "image_fail");
      }
    }
  } else if (enrollStep == 1) {             // 等待抬手（避免两次采到同一根手指）
    uint8_t r = finger.getImage();
    if (r == FINGERPRINT_NOFINGER) {
      enrollStep = 2;
      enrollDeadline = millis() + ENROLL_TIMEOUT_MS;
      publishEvent("enroll", "wait_finger2", enrollId, -1, nullptr);
    }
  } else {                                  // 等待第 2 次按压并合成存储
    uint8_t r = finger.getImage();
    if (r == FINGERPRINT_OK) {
      r = finger.image2Tz(2);
      if (r != FINGERPRINT_OK) { enrollBusy = false; publishEvent("enroll", "failed", enrollId, r, "image_fail"); return; }
      r = finger.createModel();
      if (r == FINGERPRINT_ENROLLMISMATCH) { enrollBusy = false; publishEvent("enroll", "failed", enrollId, -1, "mismatch"); return; }
      if (r != FINGERPRINT_OK) { enrollBusy = false; publishEvent("enroll", "failed", enrollId, r, "model_fail"); return; }
      r = finger.storeModel(enrollId);
      if (r != FINGERPRINT_OK) { enrollBusy = false; publishEvent("enroll", "failed", enrollId, r, "store_fail"); return; }
      fpAddEntry(enrollId, enrollNote);
      refreshFpCount();
      publishEvent("enroll", "done", enrollId, -1, enrollNote.c_str());
      enrollBusy = false;
    }
  }
}

// ============================================================
//  指纹扫描（校验模式：1:N 搜索比对）
// ============================================================
void scanFingerprint() {
  static unsigned long lastScan = 0;
  unsigned long now = millis();
  if (now - lastScan < 100) return;         // 轮询节流
  lastScan = now;

  uint8_t r = finger.getImage();
  if (r != FINGERPRINT_OK) return;          // 无手指 / 采集失败，忽略
  r = finger.image2Tz();
  if (r != FINGERPRINT_OK) return;

  r = finger.fingerFastSearch();
  if (r == FINGERPRINT_OK) {
    // 识别成功 -> 开锁
    requestUnlock("finger", finger.fingerID, finger.confidence);
  } else if (r == FINGERPRINT_NOTFOUND) {
    // 指纹存在但未匹配，节流上报
    if (now - lastNotFoundPublish > 5000) {
      lastNotFoundPublish = now;
      DynamicJsonDocument doc(128);
      doc["type"] = "scan";
      doc["result"] = "not_found";
      publishDoc(doc);
    }
  }
}

// ============================================================
//  MQTT 指令处理
// ============================================================
void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  (void)topic;
  DynamicJsonDocument doc(768);
  if (deserializeJson(doc, payload, len)) {
    Serial.println("[mqtt] 无法解析指令 JSON");
    return;
  }
  const char *type = doc["type"] | "";

  // 可选密钥校验
  if (MQTT_CMD_KEY[0] != '\0') {
    const char *key = doc["key"] | "";
    if (strcmp(key, MQTT_CMD_KEY) != 0) {
      publishError("bad_key");
      return;
    }
  }

  if (strcmp(type, "unlock") == 0) {
    requestUnlock("remote", -1, -1);
  } else if (strcmp(type, "enroll") == 0) {
    if (enrollBusy) { publishError("enroll_busy"); return; }
    int id = doc["id"] | -1;
    if (id < 0 || id >= FP_MAX_TEMPLATES) { publishError("bad_id"); return; }
    const char *note = doc["note"] | "";
    enrollBusy = true;
    enrollId = id;
    enrollNote = String(note);
    enrollStep = 0;
    enrollDeadline = millis() + ENROLL_TIMEOUT_MS;
    publishEvent("enroll", "wait_finger1", id, -1, note);
  } else if (strcmp(type, "cancel") == 0) {
    if (enrollBusy) {
      enrollBusy = false;
      publishEvent("enroll", "canceled", -1, -1, nullptr);
    }
  } else if (strcmp(type, "delete") == 0) {
    int id = doc["id"] | -1;
    if (id < 0 || id >= FP_MAX_TEMPLATES) { publishError("bad_id"); return; }
    uint8_t r = fpReady ? finger.deleteModel(id) : FINGERPRINT_PACKETRECIEVEERR;
    if (r == FINGERPRINT_OK) {
      fpRemoveEntry(id);
      refreshFpCount();
      publishEvent("delete", "ok", id, -1, nullptr);
    } else {
      publishEvent("delete", "fail", id, r, nullptr);
    }
  } else if (strcmp(type, "rename") == 0) {
    int id = doc["id"] | -1;
    const char *note = doc["note"] | "";
    if (fpRenameEntry(id, String(note))) {
      publishEvent("rename", "ok", id, -1, note);
    } else {
      publishEvent("rename", "fail", id, -1, nullptr);
    }
  } else if (strcmp(type, "clear") == 0) {
    uint8_t r = fpReady ? finger.emptyDatabase() : FINGERPRINT_PACKETRECIEVEERR;
    if (r == FINGERPRINT_OK) {
      fpClearRegistry();
      refreshFpCount();
      publishEvent("clear", "ok", -1, -1, nullptr);
    } else {
      publishEvent("clear", "fail", -1, r, nullptr);
    }
  } else if (strcmp(type, "list") == 0) {
    DynamicJsonDocument doc2(4096);
    doc2["type"] = "list";
    doc2["count"] = (int)fpRegistry.size();
    JsonArray arr = doc2.createNestedArray("items");
    for (auto &e : fpRegistry) {
      JsonObject o = arr.createNestedObject();
      o["id"] = e.id;
      o["note"] = e.note;
      o["ts"] = e.ts;
    }
    publishDoc(doc2);
  } else if (strcmp(type, "status") == 0) {
    publishStatus(true);
  }
}

// ============================================================
//  网络维护
// ============================================================
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiRetry < 5000) return;
  lastWifiRetry = millis();
  WiFi.reconnect();
  Serial.println("[wifi] 尝试重连...");
}

void mqttConnect() {
  if (mqtt.connected()) return;
  if (millis() - lastMqttRetry < 5000) return;
  lastMqttRetry = millis();

  String clientId = String(MQTT_CLIENT_ID) + "-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), HEX);
  const char *willMsg = "{\"type\":\"status\",\"online\":false}";
  // 遗嘱：设备异常掉线时向 resp 主题广播 online=false
  if (mqtt.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD,
                   MQTT_RESP_TOPIC, 1, false, willMsg)) {
    mqtt.subscribe(MQTT_CMD_TOPIC);
    Serial.printf("[mqtt] 已连接 %s:%d\n", MQTT_HOST, MQTT_PORT);
    publishStatus(true);
  } else {
    Serial.printf("[mqtt] 连接失败 rc=%d\n", mqtt.state());
  }
}

void handleNetwork() {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return;
  mqttConnect();
  if (mqtt.connected()) mqtt.loop();
}

// ============================================================
//  主程序
// ============================================================
void setup() {
  Serial.begin(115200);   // 日志走 UART0（GPIO20 RX / GPIO21 TX）；USB 仅用于烧录
  delay(200);
  Serial.println();
  Serial.println("=== 智能指纹锁 ESP32-C3 启动 ===");

  pinMode(PIN_LOCK, OUTPUT);
  digitalWrite(PIN_LOCK, LOW);   // 上电默认关锁

  // 指纹模块串口
  // 注意：这里不调用 finger.begin()（库内部会以默认引脚重新 begin 串口），
  // 直接 begin Serial1 并指定 RX/TX 引脚即可，verifyPassword() 会自动同步模块
  Serial1.begin(FP_BAUD, SERIAL_8N1, PIN_FP_RX, PIN_FP_TX);
  fpReady = initFingerprint();
  fpLoadRegistry();

  // WiFi + NTP + MQTT
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // 关 WiFi 休眠，保证响应速度（代价是功耗）
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(4096);   // 容纳指纹列表等大报文
  mqtt.setKeepAlive(30);

  Serial.printf("[wifi] 正在连接 %s ...\n", WIFI_SSID);
}

void loop() {
  handleNetwork();
  tickLock();

  // 指纹模块掉线时周期重试
  if (!fpReady) {
    static unsigned long lastFpRetry = 0;
    if (millis() - lastFpRetry > 10000) {
      lastFpRetry = millis();
      fpReady = initFingerprint();
    }
  }

  if (enrollBusy) {
    enrollTick();                 // 录入期间暂停扫描
  } else if (fpReady) {
    scanFingerprint();            // 校验模式：识别成功即开锁
  }

  // 周期状态/电量上报
  if (millis() - lastBatteryReport >= BAT_REPORT_INTERVAL_MS) {
    lastBatteryReport = millis();
    publishStatus(false);
  }
}
