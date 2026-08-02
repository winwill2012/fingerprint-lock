#pragma once
/**
 * ============================================================
 * 指纹锁 ESP32-C3 固件配置
 * 使用前请修改：WiFi 账号密码、MQTT 服务器、主题、可选密钥
 * ============================================================
 */

// ---------------- 无线网络 ----------------
#define WIFI_SSID       "你的WiFi名称"
#define WIFI_PASSWORD   "你的WiFi密码"

// ---------------- MQTT ----------------
// 默认使用公共测试 Broker（broker.emqx.io），仅用于功能调试！
// 生产环境请务必换成自建 / 商业 Broker，并建议启用 TLS（端口 8883）
#define MQTT_HOST       "broker.emqx.io"
#define MQTT_PORT       1883
#define MQTT_USERNAME   ""
#define MQTT_PASSWORD   ""
#define MQTT_CLIENT_ID  "fp-lock-esp32c3"

// 主题：小程序端"设置"页默认配置与此保持一致
#define MQTT_CMD_TOPIC   "lock/esp32c3/cmd"        // 指令下发（设备订阅）
#define MQTT_RESP_TOPIC  "lock/esp32c3/cmd/resp"   // 状态/事件反馈（设备发布）

// 可选：命令密钥。不为空时，所有指令必须携带相同的 key 字段，否则拒绝执行
// 注意：MQTT 明文传输，这只是基础防护，请务必配合 TLS + 私有 Broker 使用
#define MQTT_CMD_KEY    ""

// ---------------- 引脚 ----------------
#define PIN_FP_RX   4     // 指纹模块 TX  -> 本引脚 (IO4, ESP32 串口 RX)
#define PIN_FP_TX   5     // 指纹模块 RX  <- 本引脚 (IO5, ESP32 串口 TX)
#define PIN_LOCK    0     // 电子锁控制（高电平开锁），必须经三极管/MOS/继电器驱动
#define PIN_BAT     3     // 电池电压采样 ADC（IO3 = ADC1_CH3）

#define FP_BAUD     57600 // 指纹模块波特率（海凌科 AS608/R307 默认 57600，个别为 9600）

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
#define BAT_ADC_SAMPLES      32                 // 多次采样取平均，降低 WiFi 干扰
#define BAT_REPORT_INTERVAL_MS  15000           // 状态心跳间隔

// ---------------- 指纹库 ----------------
#define FP_MAX_TEMPLATES   100      // 本固件允许的最大指纹模板数（AS608 模块上限 200）
#define ENROLL_TIMEOUT_MS  15000    // 录入流程中每一步的超时时间

#define FW_VERSION "1.0.0"
