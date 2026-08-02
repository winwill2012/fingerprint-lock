# 智能指纹锁（ESP32-C3 + 海凌科指纹模块 + 微信小程序）

基于 ESP32-C3 的指纹门锁系统，包含两部分：

1. **ESP32-C3 固件**（`firmware/`）：指纹模块工作在校验模式（1:N 比对），识别成功自动开锁；
   支持 MQTT 远程开锁与指纹管理（录入、备注、删除、清空、列表）。
2. **微信小程序**（`miniprogram/`）：MQTT 远程开锁、指纹管理、设备状态/电池电量、事件日志；
   设置页可配置 MQTT 地址、端口、TLS、用户名、密码、指令下发 Topic 等。

> 安全声明：电子锁通电时间被硬限制在 **≤500ms**（默认 300ms），固件编译期强制检查。

---

## 目录结构

```
fingerprint-lock/
├── README.md
├── firmware/                  # ESP32-C3 固件（PlatformIO 工程）
│   ├── platformio.ini
│   ├── include/config.h       # ★ 所有配置都在这里改
│   └── src/main.cpp
└── miniprogram/               # 微信小程序（原生，无需 npm）
    ├── project.config.json
    ├── app.js / app.json / app.wxss
    ├── utils/
    │   ├── mqtt.js            # 自实现轻量 MQTT 3.1.1 客户端（WebSocket）
    │   ├── mqtt-manager.js    # 连接管理 + 消息分发 + 指令封装
    │   ├── event.js           # 轻量事件总线
    │   └── util.js
    └── pages/
        ├── index/             # 开锁页（状态 + 远程开锁）
        ├── fingerprint/       # 指纹管理页
        ├── logs/              # 事件日志页
        └── settings/          # MQTT 设置页
```

---

## 一、硬件接线

| 指纹模块引脚 | 连接目标 | 说明 |
| --- | --- | --- |
| TX | ESP32-C3 **IO4** | 模块发送 → 开发板接收（UART1 RX） |
| RX | ESP32-C3 **IO5** | 开发板发送 → 模块接收（UART1 TX） |
| VCC | 3.3V / 5V（按模块规格） | 海凌科 AS608/R307 多数为 3.3V，请以实物为准 |
| GND | GND | 共地 |
| TOUCH/WAK | 悬空 | 本固件用 UART 轮询，不使用触摸引脚 |

| 其他外设 | 连接目标 | 说明 |
| --- | --- | --- |
| 电子锁控制 | **IO0** | 高电平开锁；**必须经三极管/MOS/继电器驱动**，严禁 IO 直驱大电流负载 |
| 电池采样 | **IO3** (ADC1_CH3) | 16340 电池经两个 **100KΩ** 分压（Vbat/2）后接入 |
| 电源 | 3.3V（开发板） | 指纹模块瞬间电流大，建议与大电流负载分开供电 |

```
              ESP32-C3
   ┌───────────────────────┐
   │   IO4 ◄───────────────┼─────── 指纹模块 TX
   │   IO5 ───────────────►┼─────── 指纹模块 RX
   │   IO0 ────────────────┼──────► [三极管/继电器] ──► 电子锁(高电平开锁)
   │   IO3 ◄───────────────┼─────── 100K ──┬── 100K ──► 16340 电池正极
   │                       │              GND
   └───────────────────────┘
```

**开锁驱动电路建议**（重要）：IO0 → 1KΩ 电阻 → NPN/MOS 基极/栅极，或直接接低电平触发的继电器/锁控模块；
IO0 上电瞬间为高阻输入，建议在 IO0 与 GND 之间加 **10KΩ 下拉电阻**，防止上电误触发开锁。

---

## 二、固件（firmware/）

### 2.1 环境

- VSCode + PlatformIO 插件（推荐），打开 `firmware/` 目录即自动解析工程
- 首次编译会自动下载 `espressif32` 平台与依赖库，需要联网

### 2.2 配置

编辑 `firmware/include/config.h`：

| 配置项 | 说明 |
| --- | --- |
| `WIFI_SSID` / `WIFI_PASSWORD` | WiFi 账号密码 |
| `MQTT_HOST` / `MQTT_PORT` | MQTT Broker 地址（默认公共测试 Broker `broker.emqx.io`） |
| `MQTT_USERNAME` / `MQTT_PASSWORD` | Broker 认证（可选） |
| `MQTT_CMD_TOPIC` / `MQTT_RESP_TOPIC` | 指令 / 反馈主题，需与小程序的设置一致 |
| `MQTT_CMD_KEY` | 可选命令密钥，不为空则所有指令需携带相同 `key` |
| `LOCK_PULSE_MS` | 开锁脉冲宽度，**默认 300ms，编译期强制 ≤500ms** |
| `FP_BAUD` | 指纹模块波特率，默认 57600 |

### 2.3 烧录与查看日志

```bash
cd firmware
pio run -t upload          # 编译并烧录（开发板 USB 直连电脑）
pio device monitor         # 串口日志（UART0，115200）
```

> 日志输出在 **UART0（GPIO20 RX / GPIO21 TX，115200）**，需用 USB-UART 转接器查看；
> USB 口仅用于烧录（ESP32-C3 原生 USB 串口/JTAG）。
> 注：不要给本工程添加 `ARDUINO_USB_CDC_ON_BOOT=1`，当前 Arduino 内核 2.0.17 开启后会编译失败。

### 2.4 固件功能

- **校验模式**：指纹模块以 1:N 搜索比对方式工作，手指按下 → 采集 → 搜索 → 命中即开锁；
  未命中事件以 5s 节流上报（便于小程序展示"识别失败"）。
- **开锁脉冲**：非阻塞实现，`IO0` 高电平保持 `LOCK_PULSE_MS` 后自动断电；两次开锁最小间隔 2s 防连发。
- **MQTT 指令**：见下表。
- **指纹备注**：备注与录入时间保存在 ESP32 NVS（断电不丢失）；指纹模板存在模块 Flash 中。
- **电池上报**：IO3 ADC 多次采样平均，换算为电压与百分比，随状态心跳每 15s 上报，开锁事件也会附带电量。
- **遗嘱消息**：设备异常掉线时向 resp 主题广播 `{"type":"status","online":false}`。

---

## 三、微信小程序（miniprogram/）

原生小程序，**无需 npm 安装**，MQTT 客户端为自实现的 MQTT 3.1.1（WebSocket）。

### 3.1 导入

1. 打开**微信开发者工具** → 导入项目 → 目录选择 `miniprogram/`；
2. AppID 选择"测试号"即可（`project.config.json` 中为 `touristappid`）；
3. 开发阶段：详情 → 本地设置 → 勾选 **"不校验合法域名"**（否则 `ws://` 无法连接）；
4. 基础库版本建议 ≥ 2.17.1（指纹录入备注使用了可输入弹窗 `editable`）。

### 3.2 真机注意

真机调试/发布必须满足：Broker 支持 **wss://** 且其域名已加入小程序后台
「开发管理 → 服务器域名 → Socket 合法域名」，例如 `wss://broker.emqx.io:8084`。

### 3.3 使用流程

1. 打开 **设置** 页，填写 MQTT 信息（默认演示配置：`broker.emqx.io:8083`，ws，主题 `lock/esp32c3/cmd`）→ 保存 → 连接；
2. 回到 **开锁** 页，可看到连接状态、设备在线状态、电池电量、已录指纹数，点击"远程开锁"二次确认后下发；
3. **指纹** 页可录入（自动分配槽位 + 备注）、修改备注、删除、清空；录入过程跟随设备提示"请按手指 → 移开 → 再按一次"；
4. **日志** 页记录本地事件（开锁来源、录入/删除结果、设备离线等），最多保留 100 条。

---

## 四、MQTT 协议

### 4.1 主题

| 主题 | 方向 | 用途 |
| --- | --- | --- |
| `lock/esp32c3/cmd` | 小程序 → 设备 | 指令下发（可自定义） |
| `lock/esp32c3/cmd/resp` | 设备 → 小程序 | 状态、事件、操作结果反馈（默认 = 指令主题 + `/resp`） |

### 4.2 指令格式（小程序 → 设备，JSON）

| type | 参数 | 说明 |
| --- | --- | --- |
| `unlock` | — | 远程开锁 |
| `enroll` | `id`(0~99), `note`(可选) | 开始录入指纹到槽位 id |
| `cancel` | — | 取消进行中的录入 |
| `delete` | `id` | 删除指定指纹 |
| `rename` | `id`, `note` | 修改指纹备注 |
| `clear` | — | 清空所有指纹 |
| `list` | — | 请求指纹列表 |
| `status` | — | 请求设备状态/电量 |

> 若固件设置了 `MQTT_CMD_KEY`，所有指令需额外携带 `"key":"你的密钥"`。

### 4.3 反馈格式（设备 → 小程序，JSON）

| type | 关键字段 | 说明 |
| --- | --- | --- |
| `status` | `online`, `battery_mv`, `battery_pct`, `fp_count`, `ip`, `rssi`, `uptime` | 状态心跳（15s） |
| `unlock` | `source`(`finger`/`remote`), `fp_id`, `confidence`, `battery_pct` | 开锁事件 |
| `scan` | `result`(`not_found`) | 指纹未匹配 |
| `enroll` | `phase`: `wait_finger1`→`finger1_ok`→`lift_finger`→`wait_finger2`→`done`/`failed`/`canceled` | 录入进度 |
| `delete` / `rename` / `clear` | `phase`: `ok`/`fail`, `id` | 操作结果 |
| `list` | `items`: `[{id,note,ts}]`, `count` | 指纹列表 |
| `error` | `phase`: `bad_key`/`enroll_busy`/`bad_id` 等 | 错误码 |

---

## 五、注意事项（请务必阅读）

1. **开锁脉冲 ≤500ms**：`LOCK_PULSE_MS` 默认 300ms，`config.h` 中有编译期 `#error` 保护，切勿私自调大，否则电子锁可能烧毁。
2. **驱动电路**：电子锁电流大（电磁铁可到安培级），IO0 必须经过三极管/MOS/继电器驱动，严禁 GPIO 直驱；建议 IO0 加 10K 下拉电阻防上电误触发。
3. **指纹模块**：
   - 本固件适用于海凌科 AS608 / R307 / FPM10A 等 **8 字节 UART 协议**模块；若你的模块是 GT-521F 等其他协议，需更换驱动库；
   - 模块供电按实物规格（多数 3.3V）；VCC 与 ESP32 建议由同一直流源供电并共地；
   - 波特率默认 57600，个别模块为 9600（改 `FP_BAUD`）。
4. **电池采样**：IO3 读到的是分压后的电压，固件按 ×2 换算；WiFi 开启时 ADC 存在噪声，已做 32 次平均，电量仅作参考。16340 是 3.7V 锂电池，放电平台约 3.0~4.2V。
5. **MQTT 安全**：公共 Broker（如 broker.emqx.io）为明文传输，**仅限测试**；生产环境请使用自建/商业 Broker + TLS + 用户密码 + 命令密钥，防止他人远程开锁。
6. **小程序域名**：真机必须配置 Socket 合法域名（wss）；开发工具可临时关闭校验。
   MQTT-over-WebSocket 需要 Broker 启用 WebSocket 监听（EMQX 默认 8083/8084，Mosquitto 需配置），
   客户端已自动携带 `Sec-WebSocket-Protocol: mqtt` 子协议头（缺少该头 EMQX 会返回 400）。
7. **功耗**：本固件保持 WiFi 常连，整机电流较大，不适合长期纯电池供电；如需低功耗，可后续扩展 deep-sleep + 触摸唤醒方案。
8. **指纹数量**：固件限制最多 100 个模板（AS608 模块上限 200），超出会拒绝录入。

---

## 六、常见问题

| 现象 | 排查方向 |
| --- | --- |
| 串口打印 `指纹模块未响应` | 检查 RX/TX 是否接反（TX→IO4、RX→IO5）、波特率、模块供电、共地 |
| 指纹识别不成功 | 重新录入（两次按压需同一根手指且角度接近）；确认录入时"移开手指"步骤完成 |
| 小程序连不上 MQTT | 勾选"不校验合法域名"；确认端口与 TLS 匹配（8083=ws，8084=wss）；Broker 是否支持 WebSocket |
| 远程开锁没反应 | 确认设备在线（状态心跳 15s）；检查 cmd/resp 主题是否两端一致；`MQTT_CMD_KEY` 是否一致 |
| 电量显示异常 | WiFi 开启时 ADC 有噪声，多观察几秒；确认两个 100K 分压正常、IO3 未被占用 |
| 上电自动开锁 | 检查 IO0 驱动电路是否默认高电平，IO0 与 GND 之间加 10K 下拉 |

---

## 七、后续可扩展方向

- 低功耗：deep-sleep + 指纹模块 TOUCH 引脚中断唤醒，心跳改长周期；
- 安全：MQTT over TLS（8883）、每条指令加时间戳防重放、小程序侧登录鉴权；
- 功能：密码键盘开锁（蓝牙/WiFi 键盘）、开门记录云端存储、OTA 升级。
