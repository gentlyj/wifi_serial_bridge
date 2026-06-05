[English](README.md) | [中文](README_zh.md)

# StickS3 WiFi 串口桥接器

M5Stack StickS3 (ESP32-S3) 的 WiFi 串口桥接固件。将 StickS3 通过 UART 连接到调试目标设备，然后从 PC 或手机通过浏览器 WebSocket 终端无线访问。

## 功能

- **WiFi 配网** — 热点门户（SoftAP）首次配网，WiFi 凭据重启后自动保留
- **WebSocket 桥接** — 浏览器通过 WebSocket 连接 StickS3，数据在浏览器和 UART 之间双向转发
- **波特率切换** — 支持 1M / 1.5M，运行时通过正面按钮切换
- **LVGL UI** — 135x240 ST7789 屏幕显示连接状态、IP、波特率、引脚映射
- **PC 工具** — `pc_tool.html` 提供 HEX/ASCII 显示、时间戳、LED 活动指示灯，支持手机浏览器

## 硬件

- **开发板**: M5Stack StickS3 (ESP32-S3)
- **UART 引脚**: GPIO4 (TX), GPIO5 (RX)
- **按键**: 正面按键 (BtnA, GPIO11) — 主要操作；侧面按键 (BtnB, GPIO12) — 次要操作

## 按键映射

| 状态 | 正面按键 (A) | 侧面按键 (B) |
|------|------------|------------|
| WiFi 未连接 | — | 长按: 启动配网 |
| 配网中 | — | — |
| 等待 WS 连接 | 点击: 切换波特率 | 长按: 断开 WiFi |
| 桥接中 | 点击: 切换波特率 | 长按: 断开 WiFi |
| 桥接中 | 长按: 断开 WS 连接 | — |

## UI 状态

1. **WiFi 未连接** — 提示长按侧面按键进入配网
2. **配网中** — 显示热点名称 (S3-Setup)、密码、门户地址 (192.168.4.1)
3. **等待连接** — 显示 IP 地址、端口 (8080)、当前波特率
4. **桥接中** — 显示 IP、波特率、引脚映射 (RX→G5, TX→G4)、WS 连接指示灯（灰色=未连接，绿色=已连接）

## 编译方法

需要 ESP-IDF v5.2.3。

```bash
# 配置好 ESP-IDF 环境后：
idf.py build
idf.py -p /dev/ttyUSBx flash monitor
```

### 自定义分区表

项目使用自定义分区表 (`partitions.csv`)，应用分区约 1.95MB，以容纳完整固件。

## 使用方法

1. **首次启动**：长按侧面按键 → 手机/PC 连接 `S3-Setup` 热点（密码：`12345678`）→ 浏览器打开 `192.168.4.1` → 输入 WiFi 信息
2. **配网后**：StickS3 每次开机会自动连接已保存的 WiFi，屏幕显示 IP
3. **连接**：浏览器打开 `pc_tool.html` → 输入 `ws://<StickS3的IP>:8080` → 点击 Connect
4. **使用**：串口数据在浏览器和 UART 连接的调试目标之间双向流动

## 项目结构

```
├── CMakeLists.txt
├── partitions.csv              自定义分区表 (2MB flash)
├── sdkconfig.defaults          编译配置
├── pc_tool.html                浏览器串口终端
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml       组件依赖
│   ├── main.cpp                应用入口、状态机、事件循环
│   ├── sticks3_power.*         电源管理、电池
│   ├── sticks3_display.*       ST7789 LCD 初始化
│   ├── sticks3_button.*        GPIO 按键输入 (LVGL indev)
│   ├── sticks3_audio.*         扬声器 / 音调生成
│   ├── sticks3_nvs.*           NVS 存储 (波特率持久化)
│   ├── sticks3_wifi.*          WiFi STA 管理
│   ├── sticks3_provision.*     SoftAP 热点门户 + DNS 重定向
│   ├── sticks3_tcp_server.*    WebSocket 服务器 (端口 8080)
│   ├── sticks3_uart_bridge.*   UART1 桥接 (GPIO4/5, 8N1)
│   └── sticks3_ui.*            LVGL 界面 (4 个状态)
└── README.md
```

## 架构

```
浏览器  ←──WebSocket──→  StickS3  ←──UART1──→  调试目标
(pc_tool.html)          (ESP32-S3)             (GPIO4 TX, GPIO5 RX)
```

- **Stream Buffer**: 两个 4KB FreeRTOS stream buffer 连接 WebSocket 和 UART 任务
- **UART 任务**: 固定在 Core 1 运行（避免与 Core 0 的 WiFi 冲突），优先级 7
- **WebSocket**: 单客户端，支持二进制帧、close/ping/pong 控制帧
