#pragma once
/**
 * ============================================================
 * 指纹锁 ESP32-C3 固件配置
 * WiFi 通过手机 App BLE 配网写入 NVS，此处不再硬编码账号密码
 * ============================================================
 */

// ---------------- MQTT ----------------
// App 端同样使用 TCP：tcp://iot.welinklab.com:1883
#define MQTT_HOST       "iot.welinklab.com"
#define MQTT_PORT       1883
#define MQTT_USERNAME   "wl_wdjbr6wp2tzaq"
#define MQTT_PASSWORD   "VVwFcmYIB8kaE1GX_1mb2qHH4ZLQsE_P"
#define MQTT_CLIENT_ID  "fp-lock-esp32c3"

// 主题：与 App「设置」页默认配置保持一致
#define MQTT_CMD_TOPIC   "welink/wl_wdjbr6wp2tzaq/fingerprint-lock-cmd"
#define MQTT_RESP_TOPIC  "welink/wl_wdjbr6wp2tzaq/fingerprint-lock-upload"

// 可选：命令密钥。不为空时，所有指令必须携带相同的 key 字段，否则拒绝执行
#define MQTT_CMD_KEY    ""

// ---------------- 引脚 ----------------
#define PIN_FP_RX   4     // 指纹模块 TX  -> 本引脚 (IO4, UART1 RX)
#define PIN_FP_TX   5     // 指纹模块 RX  <- 本引脚 (IO5, UART1 TX)
#define PIN_LOCK    0     // 电子锁控制（高电平开锁），必须经三极管/MOS/继电器驱动
#define PIN_BAT     3     // 电池电压采样 ADC（IO3 = ADC1_CH3）

#define FP_BAUD     57600 // 海凌科 ZW101 默认波特率 57600

// ---------------- WiFi / RF ----------------
#define WIFI_CONNECT_TIMEOUT_MS    30000
#define WIFI_RETRY_INTERVAL_MS     8000

// ---------------- 开锁 ----------------
// ★★★ 电子锁通电时间不得超过 500ms，默认 300ms 留出安全余量 ★★★
#define LOCK_PULSE_MS        300
#if LOCK_PULSE_MS > 500
#error "LOCK_PULSE_MS 不能超过 500ms！电子锁会烧毁"
#endif
#define UNLOCK_COOLDOWN_MS   2000   // 两次开锁最小间隔，防止手指停留重复触发/连发

// ---------------- 电池 ----------------
// 16340 锂电池（3.0V ~ 4.2V），两个 100K 电阻分压 = Vbat / 2
#define BAT_DIVIDER          2.0f
#define BAT_FULL_MV          4200
#define BAT_EMPTY_MV         3000
#define BAT_ADC_SAMPLES      32
#define BAT_REPORT_INTERVAL_MS  8000   // 8s 心跳；App 15s 无上报则离线

// ---------------- 指纹库 ----------------
#define FP_MAX_TEMPLATES   50       // ZW101 规格书容量为 50 枚
#define ENROLL_TIMEOUT_MS  15000

#define FW_VERSION "1.4.13"
