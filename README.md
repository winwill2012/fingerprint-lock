# 智能保险柜

基于 **ESP32-C3**、**海凌科 ZW101** 指纹模块与 **Android 原生 App** 的智能开锁方案：本地指纹识别开锁，手机端远程开锁与指纹管理，支持 BLE 配网与 MQTT 通信。

| 端 | 目录 | 技术栈 |
| --- | --- | --- |
| 固件 | `firmware/` | PlatformIO · ESP32-C3 · Arduino |
| App | `android/` | Android Studio · Kotlin · MQTT（TCP 1883） |

App 名称：**智能保险柜**（包名 `com.fingerprintlock.app`，最低 Android 8.0 / API 26）。

---

## 功能概览

**设备端**

- 指纹 1:N 识别成功后开锁（电磁锁脉冲，硬限制 ≤500ms）
- 最多 50 枚指纹模板；录入 / 删除 / 清空 / 备注
- BLE 配网（设备名 `FPLock-XXXX`），WiFi 凭据写入 NVS
- MQTT 上报状态、电量、开锁事件；接收远程指令
- 可选待机呼吸灯；识别成功 / 失败、录入过程仍有灯效提示

**手机端**

- 首页：设备在线状态、电量、远程开锁、最近事件
- 指纹：列表管理、录入引导、改备注、删除、清空
- 设置：MQTT 参数、呼吸灯开关、BLE 配网

---

## 仓库结构

```
fingerprint-lock/
├── firmware/          # 门锁固件（PlatformIO）
│   ├── include/       # config.h 引脚 / MQTT / 安全参数
│   ├── src/           # main.cpp、ble_prov 等
│   └── platformio.ini
├── android/           # Android Studio 工程（请打开此目录）
│   ├── app/
│   └── asserts/       # 设计资源（Logo、Tab 图标等）
└── README.md
```

---

## 硬件连接

| 功能 | 引脚 | 说明 |
| --- | --- | --- |
| 指纹 TX → ESP RX | IO4 | ZW101，57600 baud |
| 指纹 RX ← ESP TX | IO5 | |
| 电子锁 | IO0 | **高电平开锁**，须经三极管 / MOS / 继电器驱动，禁止直驱 |
| 电池采样 | IO3 | 分压后 ADC（默认按 16340：3.0V～4.2V） |

其他约定：

- WiFi 发射功率固定 **8.5dBm**
- 开锁脉冲默认 **300ms**（编译期禁止超过 500ms）
- 固件版本见 `firmware/include/config.h` 中 `FW_VERSION`

---

## 固件（`firmware/`）

### 编译与烧录

```bash
cd firmware
# 按需编辑 include/config.h（MQTT 默认已填；WiFi 由 App BLE 配网）
pio run -t upload
pio device monitor   # 可选：查看日志
```

### 主要能力

- MQTT JSON 指令：`unlock` / `enroll` / `cancel` / `delete` / `rename` / `clear` / `list` / `status` / `led`
- 指纹注册表保存在 NVS（备注、时间戳）；列表以注册表为准，避免 UART 偶发读失败导致列表乱跳
- 删除 / 清空会校验模块 Flash，避免「App 列表空了还能开锁」
- BLE 与 WiFi 共存时使用 `WIFI_PS_MIN_MODEM`，避免 ESP32-C3 异常复位

---

## Android App（`android/`）

### 打开工程

1. 安装 [Android Studio](https://developer.android.com/studio)（建议较新稳定版）
2. **File → Open** → 选择本仓库的 **`android/`** 目录（不要打开仓库根目录）
3. 等待 Gradle Sync
4. 连接真机运行（BLE 配网需要真机蓝牙）

### 页面说明

| 页面 | 功能 |
| --- | --- |
| 首页 | 在线状态、电量、远程开锁按钮、最近开锁 / 事件 |
| 指纹 | 录入、备注、删除、清空；列表与设备注册表同步 |
| 设置 | MQTT Host/Port/账号/Topic、呼吸灯、进入 BLE 配网 |

### 配网流程

1. 烧录固件，设备广播 `FPLock-XXXX`
2. App → 设置 → **BLE 配网** → 授予蓝牙 / 定位（及 Android 12+「附近的设备」）权限
3. 扫描并连接设备 → 选择 **2.4GHz WiFi**、输入密码 → 下发
4. 配网成功后，App 通过 MQTT 与设备通信即可远程开锁、管理指纹

### 默认 MQTT（可在设置中修改）

| 项 | 默认值 |
| --- | --- |
| 协议 | TCP `iot.welinklab.com:1883`（明文 MQTT，非 WSS） |
| 用户 | 见 `config.h` / App 设置页默认值 |
| 指令 Topic | `welink/.../fingerprint-lock-cmd` |
| 上报 Topic | `welink/.../fingerprint-lock-upload` |

> 账号密码写在工程默认配置中，若仓库公开请务必更换。

---

## MQTT 协议摘要

**App → 设备（cmd）** 常见字段：`type`，以及按需的 `id` / `note` / `enable` 等。

| type | 作用 |
| --- | --- |
| `unlock` | 远程开锁 |
| `enroll` | 录入指纹（`id`、`note`） |
| `cancel` | 取消录入 |
| `delete` / `clear` | 删除单枚 / 清空全部 |
| `rename` | 修改备注 |
| `list` / `status` | 拉取列表 / 状态 |
| `led` | 开关待机呼吸灯 |

**设备 → App（upload）**：`status`、`list`、`enroll` 过程相位、`unlock` 事件、`delete`/`clear`/`rename` 回执、`error` 等。

---

## 注意事项

1. **开锁脉冲 ≤500ms**，IO0 必须经驱动电路接锁，否则可能烧毁模块或锁体  
2. 指纹模块与灯控共用 UART，忙时不要依赖全量扫库刷新列表  
3. Android 12+ BLE 扫描需要「附近的设备」与定位等相关权限  
4. 仅支持 2.4GHz WiFi；配网设备名形如 `FPLock-XXXX`  
5. 公开或量产前更换 MQTT 账号、密码，并视需要启用 `MQTT_CMD_KEY`

---

## 许可与声明

本仓库为智能柜 / 门锁类演示与产品原型代码。硬件选型、锁体驱动与安规需按实际产品自行评估与测试。
