# StickS3 Bridge

ESP32-based WiFi serial bridge. The ESP32 connects to a target device via UART, and remote clients connect via WebSocket. The ESP32 forwards serial data bidirectionally between UART and WebSocket.

## Architecture

- UART <-> ESP32 <-> WebSocket clients
- HTTP server on port 80 serves a web-based serial tool (`pc_tool.html`)
- WebSocket endpoint `/ws` handles real-time serial data forwarding

## Key Files

- `main/sticks3_http_server.c` — HTTP server, WebSocket handler, UART bridge logic. Embedded HTML is stored in `TOOL_HTML` string constant.
- `pc_tool.html` — Standalone copy of the web serial tool (source of truth for editing, must be synced back to `TOOL_HTML` in the C file after changes)
- `main/main.c` — Entry point, launches WiFi and HTTP server tasks

## Build

ESP-IDF v5.2.3. Use `idf.py build` / `idf.py flash`.

## Sync Rule

When editing the web UI: edit `pc_tool.html` first, then copy its content into `sticks3_http_server.c`'s `TOOL_HTML` constant to keep them in sync.
