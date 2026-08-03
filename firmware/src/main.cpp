/**
 * ============================================================
 * 智能指纹锁固件 —— ESP32-C3
 * ------------------------------------------------------------
 * 硬件：
 *   - 指纹模块（海凌科 ZW101，兼容 AS608 类 UART 协议，默认 57600）
 *      模块 TX -> IO4 (Serial1 RX)
 *      模块 RX <- IO5 (Serial1 TX)
 *   - 电子锁：IO0 高电平开锁，脉冲宽度 LOCK_PULSE_MS（默认 300ms，≤500ms）
 *   - 电池：IO3 (ADC1_CH3) 经两个 100K 分压采样 16340 电池电压
 * 功能：
 *   - 指纹模块工作在校验模式（1:N 搜索比对），识别成功即开锁
 *   - 手机 App 低功耗蓝牙（BLE）配网，WiFi 凭据存 NVS
 *   - WiFi 发射功率固定 8.5dBm
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
#include <algorithm>
#include "config.h"
#include "ble_prov.h"

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
static volatile bool enrollCancelReq = false;
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
static uint32_t fpListSeq = 0;

// ---------------- 其他状态 ----------------
static unsigned long lastBatteryReport = 0;
static unsigned long lastNotFoundPublish = 0;
static unsigned long lastWifiRetry = 0;
static unsigned long lastMqttRetry = 0;
static bool fpReady = false;
static bool ledBreathEnabled = true;  // 仅控制待机呼吸灯；识别/录入提示灯始终保留
static bool fpCmdBusy = false;        // 发灯控时暂停扫描，避免 UART 抢占
static unsigned long lastLedRefreshMs = 0;
static uint8_t lastLedModeRc = 0xFF;
static uint8_t lastLedBlnRc = 0xFF;
static bool ledEffectActive = false;  // 识别/录入提示灯效播放中
static unsigned long ledEffectUntilMs = 0;

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
  if (n == 0 || n >= sizeof(buf)) {
    Serial.println("[mqtt] serialize overflow");
    return;
  }
  // 列表等大包偶发失败时重试，避免 App 收到残缺/丢包后条数跳变
  for (int i = 0; i < 3; i++) {
    if (mqtt.publish(MQTT_RESP_TOPIC, (const uint8_t *)buf, n, false)) return;
    delay(20);
  }
  Serial.printf("[mqtt] publish fail len=%u\n", (unsigned)n);
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
  doc["fp_count"] = (int)fpRegistry.size();  // 以注册表为准，不用模块 templateCount（会抖动）
  doc["fp_reg"] = (int)fpRegistry.size();
  if (fpReady) {
    // 仅作参考，不驱动 App 列表
    doc["fp_tpl"] = finger.templateCount;
  }
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = (int)(millis() / 1000);
  doc["enroll_busy"] = enrollBusy;
  doc["led_breath"] = ledBreathEnabled;
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

bool fpIdInRegistry(int id) {
  for (auto &e : fpRegistry) {
    if (e.id == id) return true;
  }
  return false;
}

/** 槽位是否占用。UART 通信失败时按占用处理，避免误覆盖已有模板。 */
bool fpSlotTaken(int id) {
  if (fpIdInRegistry(id)) return true;
  if (!fpReady) return false;
  uint8_t r = finger.loadModel(id);
  if (r == FINGERPRINT_OK) return true;
  if (r == FINGERPRINT_PACKETRECIEVEERR || r == FINGERPRINT_TIMEOUT ||
      r == FINGERPRINT_BADPACKET) {
    return true;
  }
  return false;
}

int fpFindFreeId() {
  for (int i = 0; i < FP_MAX_TEMPLATES; i++) {
    if (!fpSlotTaken(i)) return i;
  }
  return -1;
}

/** 确认槽位已空。通信失败视为未确认（不能当已删除）。 */
bool fpSlotVerifiedEmpty(int id) {
  if (!fpReady) return false;
  uint8_t r = finger.loadModel(id);
  if (r == FINGERPRINT_OK) return false;
  if (r == FINGERPRINT_PACKETRECIEVEERR || r == FINGERPRINT_TIMEOUT ||
      r == FINGERPRINT_BADPACKET) {
    return false;
  }
  return true;
}

/** 删除单个模板并校验；失败不改注册表。 */
bool fpDeleteTemplate(int id) {
  if (!fpReady) return false;
  for (int attempt = 0; attempt < 3; attempt++) {
    uint8_t r = finger.deleteModel((uint16_t)id);
    delay(40);
    if (fpSlotVerifiedEmpty(id)) return true;
    // 部分模块删空槽也回 OK，再确认一次
    if (r == FINGERPRINT_OK) {
      delay(40);
      if (fpSlotVerifiedEmpty(id)) return true;
    }
  }
  return fpSlotVerifiedEmpty(id);
}

/** 清空模块全部模板：emptyDatabase + 扫残留逐个删，最后校验。 */
bool fpWipeModule() {
  if (!fpReady) return false;

  uint8_t r = finger.emptyDatabase();
  Serial.printf("[fp] emptyDatabase -> 0x%02X\n", r);
  delay(150);

  // 再扫一遍，清掉 empty 未删净的残留（含注册表外的孤儿模板）
  int left = 0;
  for (int id = 0; id < FP_MAX_TEMPLATES; id++) {
    uint8_t lr = finger.loadModel(id);
    if (lr == FINGERPRINT_OK) {
      if (!fpDeleteTemplate(id)) left++;
    }
  }

  refreshFpCount();
  if (finger.templateCount > 0) {
    Serial.printf("[fp] wipe: templateCount still %d\n", finger.templateCount);
    // templateCount 偶发不准，以 loadModel 为准再扫一次
    left = 0;
    for (int id = 0; id < FP_MAX_TEMPLATES; id++) {
      if (finger.loadModel(id) == FINGERPRINT_OK) left++;
    }
  }

  Serial.printf("[fp] wipe done, residual=%d tpl=%d\n", left, finger.templateCount);
  return left == 0;
}

void refreshFpCount() {
  if (fpReady) finger.getTemplateCount();
}

/** 从模块补齐注册表：只增加确认存在的槽位，绝不因读失败删掉已有项。
 *  全量重建会在 UART 偶发失败时造成列表时而缺 0、时而缺 1。 */
void fpSyncFromModule() {
  if (!fpReady) return;
  if (fpCmdBusy || enrollBusy) {
    Serial.println("[fp] sync skip: uart busy");
    return;
  }

  int added = 0;
  int seen = 0;
  int failLoads = 0;
  bool dirty = false;

  for (int id = 0; id < FP_MAX_TEMPLATES; id++) {
    if (fpCmdBusy || enrollBusy) {
      Serial.println("[fp] sync abort mid-way: uart busy");
      break;
    }
    uint8_t r = finger.loadModel(id);
    if (r == FINGERPRINT_OK) {
      seen++;
      bool exists = false;
      for (auto &e : fpRegistry) {
        if (e.id == id) { exists = true; break; }
      }
      if (!exists) {
        FpEntry e;
        e.id = id;
        e.note = "指纹#" + String(id);
        e.ts = nowSec();
        fpRegistry.push_back(e);
        added++;
        dirty = true;
      }
    } else if (r == FINGERPRINT_PACKETRECIEVEERR || r == FINGERPRINT_TIMEOUT ||
               r == FINGERPRINT_BADPACKET) {
      failLoads++;
    }
    // 其它返回值视为「空槽或不确定」，保留注册表原样，绝不删除
  }

  if (dirty) fpSaveRegistry();
  refreshFpCount();
  Serial.printf("[fp] sync merge seen=%d added=%d fail=%d stored=%d\n",
                seen, added, failLoads, (int)fpRegistry.size());
}

void publishFpList() {
  // 按 id 排序，避免 App 显示顺序跳动
  std::sort(fpRegistry.begin(), fpRegistry.end(),
            [](const FpEntry &a, const FpEntry &b) { return a.id < b.id; });

  fpListSeq++;
  DynamicJsonDocument doc2(4096);
  doc2["type"] = "list";
  doc2["seq"] = fpListSeq;
  doc2["count"] = (int)fpRegistry.size();
  JsonArray arr = doc2.createNestedArray("items");
  for (auto &e : fpRegistry) {
    JsonObject o = arr.createNestedObject();
    o["id"] = e.id;
    o["note"] = e.note;
    o["ts"] = e.ts;
  }
  publishDoc(doc2);
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
//  指纹指示灯（海凌科：PS_BlnAmSw=0x60 / PS_ControlBLN=0x3C）
//  - 开启呼吸：自动模式（待机蓝灯呼吸 + 模块自带识别/录入灯效）
//  - 关闭呼吸：手动模式待机熄灭，但识别成功/失败、录入过程仍手动播提示灯
// ============================================================
void fpFlushRx() {
  while (Serial1.available() > 0) {
    (void)Serial1.read();
  }
}

uint8_t fpSendCmd(uint8_t *data, uint8_t n) {
  fpFlushRx();
  Adafruit_Fingerprint_Packet packet(FINGERPRINT_COMMANDPACKET, n, data);
  finger.writeStructuredPacket(packet);
  uint8_t r = finger.getStructuredPacket(&packet, 1500);
  if (r != FINGERPRINT_OK) return r;
  if (packet.type != FINGERPRINT_ACKPACKET) return FINGERPRINT_PACKETRECIEVEERR;
  return packet.data[0];
}

uint8_t fpBlnAmSw(uint8_t mode) {
  uint8_t d[] = {0x60, mode};
  return fpSendCmd(d, sizeof(d));
}

/** 普通三色灯：func 01呼吸/02闪烁/03常开/04常闭；颜色 bit0蓝 bit1绿 bit2红 */
uint8_t fpControlBlnBasic(uint8_t func, uint8_t startColor, uint8_t endColor, uint8_t cycles) {
  uint8_t d[] = {0x3C, func, startColor, endColor, cycles};
  return fpSendCmd(d, sizeof(d));
}

uint8_t fpControlBlnExt(uint8_t func, uint8_t startColor, uint8_t endOrDuty,
                        uint8_t cycles, uint8_t timeTenths) {
  uint8_t d[] = {0x3C, func, startColor, endOrDuty, cycles, timeTenths};
  return fpSendCmd(d, sizeof(d));
}

void ledLoadPref() {
  prefs.begin("cfg", true);
  ledBreathEnabled = prefs.getBool("led_breath", true);
  prefs.end();
}

void ledSavePref() {
  prefs.begin("cfg", false);
  prefs.putBool("led_breath", ledBreathEnabled);
  prefs.end();
}

/** 恢复待机灯效：开=立即蓝灯呼吸；关=待机熄灭（识别提示灯另播） */
bool applyLedIdle() {
  if (!fpReady) return false;
  fpCmdBusy = true;
  delay(15);
  fpFlushRx();

  // 统一手动模式，便于立即生效（自动模式切呼吸通常要等业务结束/上电）
  lastLedModeRc = fpBlnAmSw(0x00);
  delay(30);
  fpFlushRx();

  if (ledBreathEnabled) {
    // 立刻开启蓝灯呼吸（无限循环）
    lastLedBlnRc = fpControlBlnBasic(0x01, 0x01, 0x01, 0x00);
    delay(30);
    fpFlushRx();
    uint8_t r2 = fpControlBlnExt(0x01, 0x01, 0x01, 0x00, 18);
    if (lastLedBlnRc != FINGERPRINT_OK && r2 == FINGERPRINT_OK) lastLedBlnRc = r2;
  } else {
    lastLedBlnRc = fpControlBlnBasic(0x04, 0x00, 0x00, 0x00);
  }

  ledEffectActive = false;
  lastLedRefreshMs = millis();
  fpCmdBusy = false;
  Serial.printf("[led] idle breath=%d BlnAmSw=0x%02X ControlBLN=0x%02X\n",
                ledBreathEnabled, lastLedModeRc, lastLedBlnRc);
  return lastLedModeRc == FINGERPRINT_OK || lastLedBlnRc == FINGERPRINT_OK;
}

bool applyLedBreath() { return applyLedIdle(); }

void setLedBreath(bool on) {
  ledBreathEnabled = on;
  ledSavePref();
  bool ok = applyLedIdle();
  char note[24];
  snprintf(note, sizeof(note), "%s m%02Xb%02X", ok ? "ok" : "fail",
           lastLedModeRc, lastLedBlnRc);
  publishEvent("led", on ? "on" : "off", -1, -1, note);
  publishStatus(true);
}

/** 识别/录入提示灯：无论是否开启待机呼吸，都手动播放，结束后恢复待机 */
static void ledBeginEffect(unsigned long durationMs) {
  ledEffectActive = true;
  ledEffectUntilMs = millis() + durationMs;
  lastLedRefreshMs = millis();
}

void ledPlaySuccess() {
  if (!fpReady) return;
  fpCmdBusy = true;
  delay(10);
  fpFlushRx();
  fpControlBlnBasic(0x03, 0x02, 0x02, 0); // 绿灯常亮片刻
  fpCmdBusy = false;
  ledBeginEffect(900);
}

void ledPlayFail() {
  if (!fpReady) return;
  fpCmdBusy = true;
  delay(10);
  fpFlushRx();
  fpControlBlnBasic(0x02, 0x04, 0x04, 3); // 红灯闪 3 次
  fpCmdBusy = false;
  ledBeginEffect(1200);
}

void ledPlayWaitFinger() {
  if (!fpReady) return;
  fpCmdBusy = true;
  delay(10);
  fpFlushRx();
  fpControlBlnExt(0x02, 0x01, 0x18, 0x00, 10); // 蓝灯慢闪提示放手指
  fpCmdBusy = false;
  ledBeginEffect(20000);
}

void ledMaintain() {
  if (!fpReady || fpCmdBusy) return;

  if (ledEffectActive) {
    if ((long)(millis() - ledEffectUntilMs) >= 0) {
      ledEffectActive = false;
      if (!enrollBusy) applyLedIdle();
    }
    return;
  }

  if (enrollBusy) return;

  // 待机态周期确认呼吸/熄灭
  unsigned long interval = ledBreathEnabled ? 8000UL : 4000UL;
  if (millis() - lastLedRefreshMs < interval) return;
  applyLedIdle();
}

// ============================================================
//  指纹录入状态机（由主循环驱动）
// ============================================================
void enrollTick() {
  if (!enrollBusy) return;
  if (enrollCancelReq) {
    enrollCancelReq = false;
    enrollBusy = false;
    publishEvent("enroll", "canceled", enrollId, -1, nullptr);
    applyLedIdle();
    return;
  }

  if (millis() > enrollDeadline) {
    enrollBusy = false;
    publishEvent("enroll", "failed", enrollId, -1, "timeout");
    ledPlayFail();
    return;
  }

  if (enrollStep == 0) {                    // 等待第 1 次按压
    uint8_t r = finger.getImage();
    if (enrollCancelReq || !enrollBusy) { enrollCancelReq = false; return; }
    if (r == FINGERPRINT_OK) {
      r = finger.image2Tz(1);
      if (r == FINGERPRINT_OK) {
        enrollStep = 1;
        enrollDeadline = millis() + ENROLL_TIMEOUT_MS;
        publishEvent("enroll", "finger1_ok", enrollId, -1, nullptr);
        publishEvent("enroll", "lift_finger", enrollId, -1, nullptr);
        ledPlaySuccess(); // 短绿提示第 1 次成功（关闭呼吸时）
      } else {
        enrollBusy = false;
        publishEvent("enroll", "failed", enrollId, r, "image_fail");
        ledPlayFail();
      }
    }
  } else if (enrollStep == 1) {             // 等待抬手（避免两次采到同一根手指）
    uint8_t r = finger.getImage();
    if (enrollCancelReq || !enrollBusy) { enrollCancelReq = false; return; }
    if (r == FINGERPRINT_NOFINGER) {
      enrollStep = 2;
      enrollDeadline = millis() + ENROLL_TIMEOUT_MS;
      publishEvent("enroll", "wait_finger2", enrollId, -1, nullptr);
      ledPlayWaitFinger();
    }
  } else {                                  // 等待第 2 次按压并合成存储
    uint8_t r = finger.getImage();
    if (enrollCancelReq || !enrollBusy) { enrollCancelReq = false; return; }
    if (r == FINGERPRINT_OK) {
      r = finger.image2Tz(2);
      if (r != FINGERPRINT_OK) {
        enrollBusy = false;
        publishEvent("enroll", "failed", enrollId, r, "image_fail");
        ledPlayFail();
        return;
      }
      r = finger.createModel();
      if (r == FINGERPRINT_ENROLLMISMATCH) {
        enrollBusy = false;
        publishEvent("enroll", "failed", enrollId, -1, "mismatch");
        ledPlayFail();
        return;
      }
      if (r != FINGERPRINT_OK) {
        enrollBusy = false;
        publishEvent("enroll", "failed", enrollId, r, "model_fail");
        ledPlayFail();
        return;
      }
      r = finger.storeModel(enrollId);
      if (r != FINGERPRINT_OK) {
        enrollBusy = false;
        publishEvent("enroll", "failed", enrollId, r, "store_fail");
        ledPlayFail();
        return;
      }
      fpAddEntry(enrollId, enrollNote);
      refreshFpCount();
      publishEvent("enroll", "done", enrollId, -1, enrollNote.c_str());
      // 立刻推列表，避免 App 再拉 list 触发全量 sync 把刚写入的槽位「扫丢」
      publishFpList();
      enrollBusy = false;
      ledPlaySuccess();
    }
  }
}

// ============================================================
//  指纹扫描（校验模式：1:N 搜索比对）
// ============================================================
void scanFingerprint() {
  if (fpCmdBusy) return;
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
    ledPlaySuccess();
  } else if (r == FINGERPRINT_NOTFOUND) {
    // 指纹存在但未匹配，节流上报
    if (now - lastNotFoundPublish > 5000) {
      lastNotFoundPublish = now;
      DynamicJsonDocument doc(128);
      doc["type"] = "scan";
      doc["result"] = "not_found";
      publishDoc(doc);
    }
    ledPlayFail();
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
    // 若上一次未正常结束，允许强制重新开始，避免卡在 busy
    enrollCancelReq = false;
    int id = doc["id"] | -1;
    if (id < 0 || id >= FP_MAX_TEMPLATES) { publishError("bad_id"); return; }
    // 注册表或模块已占用则改分空位（通信失败不当空槽，避免覆盖）
    if (fpSlotTaken(id)) {
      int freeId = fpFindFreeId();
      if (freeId < 0) { publishError("full"); return; }
      id = freeId;
    }
    const char *note = doc["note"] | "";
    enrollBusy = true;
    enrollId = id;
    enrollNote = String(note);
    enrollStep = 0;
    enrollDeadline = millis() + ENROLL_TIMEOUT_MS;
    // 立刻回执，降低 App 侧体感延迟
    publishEvent("enroll", "wait_finger1", id, -1, note);
    ledPlayWaitFinger();
  } else if (strcmp(type, "cancel") == 0) {
    enrollCancelReq = true;
    enrollBusy = false;
    publishEvent("enroll", "canceled", enrollId, -1, nullptr);
    applyLedIdle();
  } else if (strcmp(type, "delete") == 0) {
    int id = doc["id"] | -1;
    if (id < 0 || id >= FP_MAX_TEMPLATES) { publishError("bad_id"); return; }
    if (!fpReady) {
      publishEvent("delete", "fail", id, -1, "fp_not_ready");
      publishFpList();
      return;
    }
    // 必须先删模块并校验成功，才改注册表（避免 App 列表空了还能开锁）
    if (fpDeleteTemplate(id)) {
      fpRemoveEntry(id);
      refreshFpCount();
      publishEvent("delete", "ok", id, -1, nullptr);
    } else {
      publishEvent("delete", "fail", id, -1, "delete_fail");
    }
    publishFpList();
  } else if (strcmp(type, "rename") == 0) {
    int id = doc["id"] | -1;
    const char *note = doc["note"] | "";
    if (id < 0 || id >= FP_MAX_TEMPLATES) { publishError("bad_id"); return; }
    if (!fpRenameEntry(id, String(note))) {
      fpAddEntry(id, String(note));
    }
    publishEvent("rename", "ok", id, -1, note);
    publishFpList();
  } else if (strcmp(type, "clear") == 0) {
    if (!fpReady) {
      publishEvent("clear", "fail", -1, -1, "fp_not_ready");
      publishFpList();
      return;
    }
    // 暂停扫描占用，完整擦除模块后再清注册表
    bool prevBusy = fpCmdBusy;
    fpCmdBusy = true;
    bool wiped = fpWipeModule();
    fpCmdBusy = prevBusy;
    if (wiped) {
      fpClearRegistry();
      refreshFpCount();
      publishEvent("clear", "ok", -1, -1, nullptr);
    } else {
      publishEvent("clear", "fail", -1, -1, "wipe_fail");
    }
    publishFpList();
  } else if (strcmp(type, "list") == 0) {
    // 刷新只回 NVS 注册表，不再每次全量扫模块（UART 偶发失败会导致列表乱跳）
    publishFpList();
  } else if (strcmp(type, "status") == 0) {
    publishStatus(true);
  } else if (strcmp(type, "led") == 0) {
    bool on = true;
    if (doc.containsKey("enable")) on = doc["enable"].as<bool>();
    else if (doc.containsKey("on")) on = doc["on"].as<bool>();
    else {
      const char *phase = doc["phase"] | "";
      if (strcmp(phase, "off") == 0 || strcmp(phase, "0") == 0) on = false;
    }
    setLedBreath(on);
  }
}

// ============================================================
//  网络维护
// ============================================================
void applyWifiTxPower() {
  // STA 已启动后设置；部分板子需在 begin 后再次确认
  if (WiFi.getMode() & WIFI_MODE_STA) {
    WiFi.setTxPower((wifi_power_t)WIFI_TX_POWER_QUARTER_DBM);
  }
}

void startWifiFromNvs() {
  String ssid, pass;
  if (!wifiLoadCredentials(ssid, pass)) {
    Serial.println("[wifi] 无凭据，等待 App BLE 配网（设备名 FPLock-XXXX）");
    return;
  }
  WiFi.begin(ssid.c_str(), pass.c_str());
  applyWifiTxPower();
  Serial.printf("[wifi] 使用已存凭据连接 %s ...\n", ssid.c_str());
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    // 偶发功率被改写时再钉一次
    static unsigned long lastPowerCheck = 0;
    if (millis() - lastPowerCheck > 60000) {
      lastPowerCheck = millis();
      applyWifiTxPower();
    }
    return;
  }
  if (!wifiHasCredentials()) return;
  if (millis() - lastWifiRetry < WIFI_RETRY_INTERVAL_MS) return;
  lastWifiRetry = millis();
  Serial.println("[wifi] 尝试重连...");
  WiFi.disconnect(false, false);
  delay(20);
  String ssid, pass;
  if (wifiLoadCredentials(ssid, pass)) {
    WiFi.begin(ssid.c_str(), pass.c_str());
    applyWifiTxPower();
  }
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

  ledLoadPref();

  // 指纹模块串口
  // 注意：这里不调用 finger.begin()（库内部会以默认引脚重新 begin 串口），
  // 直接 begin Serial1 并指定 RX/TX 引脚即可，verifyPassword() 会自动同步模块
  Serial1.begin(FP_BAUD, SERIAL_8N1, PIN_FP_RX, PIN_FP_TX);
  fpReady = initFingerprint();
  fpLoadRegistry();
  if (fpReady) {
    fpSyncFromModule();
    applyLedIdle();
  }

  // WiFi + BLE 共存：必须开启 modem sleep，否则 ESP32-C3 会 abort 重启
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_MIN_MODEM);

  // BLE 配网（始终广播，便于首次配置与重新配网）
  bleProvBegin();

  // 从 NVS 读取 BLE 写入的凭据；发射功率 8.5dBm
  startWifiFromNvs();

  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(4096);   // 容纳指纹列表等大报文
  mqtt.setKeepAlive(30);
}

void loop() {
  bleProvLoop();
  handleNetwork();
  tickLock();
  ledMaintain();

  // 指纹模块掉线时周期重试
  if (!fpReady) {
    static unsigned long lastFpRetry = 0;
    if (millis() - lastFpRetry > 10000) {
      lastFpRetry = millis();
      fpReady = initFingerprint();
      if (fpReady) applyLedIdle();
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
