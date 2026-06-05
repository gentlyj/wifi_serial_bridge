[English](README.md) | [中文](README_zh.md)

# StickS3 WiFi Serial Bridge

A WiFi-to-UART bridge firmware for M5Stack StickS3 (ESP32-S3). Connect the StickS3 to a debug target via UART, then access it wirelessly from a PC or phone through a browser-based WebSocket terminal.

## Features

- **WiFi Provisioning** — Captive portal (SoftAP) for first-time WiFi setup, credentials persist across reboots
- **WebSocket Bridge** — Browser connects to StickS3 via WebSocket, data flows bidirectionally between browser and UART
- **Baud Rate Switching** — Supports 1M / 1.5M baud, switchable via front button at runtime
- **LVGL UI** — 135x240 ST7789 display shows connection status, IP, baud rate, and pin mapping
- **PC Tool** — `pc_tool.html` provides hex/ASCII display, timestamps, LED activity indicators, mobile-friendly

## Hardware

- **Board**: M5Stack StickS3 (ESP32-S3)
- **UART Pins**: GPIO4 (TX), GPIO5 (RX)
- **Buttons**: Front button (BtnA, GPIO11) — primary; Side button (BtnB, GPIO12) — secondary

## Button Mapping

| State | Front Btn (A) | Side Btn (B) |
|-------|--------------|--------------|
| WiFi Disconnected | — | Long press: Start provisioning |
| Provisioning | — | — |
| Waiting for WS | Click: Switch baud | Long press: Disconnect WiFi |
| Bridge Active | Click: Switch baud | Long press: Disconnect WiFi |
| Bridge Active | Long press: Disconnect WS | — |

## UI States

1. **WiFi Disconnected** — Shows hint to long press side button for setup
2. **Provisioning** — Shows AP name (S3-Setup), password, and portal URL (192.168.4.1)
3. **Waiting** — Shows IP address, port (8080), current baud rate
4. **Bridge Active** — Shows IP, baud, pin mapping (RX→G5, TX→G4), WS connection indicator (dot: gray=disconnected, green=connected)

## How to Build

Requires ESP-IDF v5.2.3.

```bash
# Set up ESP-IDF environment, then:
idf.py build
idf.py -p /dev/ttyUSBx flash monitor
```

### Custom Partition Table

The project uses a custom partition table (`partitions.csv`) with a ~1.95MB app partition to fit the firmware.

## How to Use

1. **First boot**: Long press side button → connect phone/PC to `S3-Setup` WiFi (password: `12345678`) → open `192.168.4.1` in browser → enter your WiFi credentials
2. **After provisioning**: StickS3 auto-connects to saved WiFi on boot, displays IP on screen
3. **Connect**: Open `pc_tool.html` in a browser → enter `ws://<stickS3-ip>:8080` → click Connect
4. **Use**: Serial data flows between the browser and the UART-connected debug target

## Project Structure

```
├── CMakeLists.txt
├── partitions.csv              Custom partition table (2MB flash)
├── sdkconfig.defaults          Build configuration
├── pc_tool.html                Browser-based serial terminal
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml       Component dependencies
│   ├── main.cpp                App entry, state machine, event loop
│   ├── sticks3_power.*         Power management, battery
│   ├── sticks3_display.*       ST7789 LCD init
│   ├── sticks3_button.*        GPIO button input (LVGL indev)
│   ├── sticks3_audio.*         Speaker / tone generation
│   ├── sticks3_nvs.*           NVS storage (baud rate persistence)
│   ├── sticks3_wifi.*          WiFi STA management
│   ├── sticks3_provision.*     SoftAP captive portal + DNS redirect
│   ├── sticks3_tcp_server.*    WebSocket server (port 8080)
│   ├── sticks3_uart_bridge.*   UART1 bridge (GPIO4/5, 8N1)
│   └── sticks3_ui.*            LVGL UI for 4 states
└── README.md
```

## Architecture

```
Browser  ←──WebSocket──→  StickS3  ←──UART1──→  Debug Target
(pc_tool.html)           (ESP32-S3)             (GPIO4 TX, GPIO5 RX)
```

- **Stream Buffers**: Two 4KB FreeRTOS stream buffers connect WebSocket ↔ UART tasks
- **UART Tasks**: Pinned to Core 1 (avoids WiFi on Core 0), priority 7
- **WebSocket**: Single-client, handles binary frames, close/ping/pong
