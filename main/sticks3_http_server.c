#include "sticks3_http_server.h"
#include "sticks3_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <errno.h>

static const char *TAG = "sticks3_http";

static TaskHandle_t s_http_task = NULL;
static int s_listen_sock = -1;
static bool s_running = false;
static bool s_ws_connected = false;

// ─── Embedded HTML (placeholder %%IP%% will be replaced with device IP) ───

static const char TOOL_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no\">\n"
"<title>StickS3 Serial Bridge</title>\n"
"<style>\n"
"* { margin: 0; padding: 0; box-sizing: border-box; }\n"
"body {\n"
"    font-family: 'Consolas', 'Monaco', 'Courier New', monospace;\n"
"    background: #1a1a2e; color: #eee;\n"
"    height: 100vh; height: 100dvh;\n"
"    display: flex; flex-direction: column;\n"
"}\n"
".header {\n"
"    background: #16213e; padding: 10px 16px; display: flex;\n"
"    align-items: center; gap: 12px; flex-shrink: 0;\n"
"    border-bottom: 1px solid #0f3460;\n"
"}\n"
".header h1 { font-size: 16px; color: #e94560; white-space: nowrap; }\n"
".status {\n"
"    display: inline-block; width: 10px; height: 10px;\n"
"    border-radius: 50%; background: #666; margin-right: 4px;\n"
"    flex-shrink: 0;\n"
"}\n"
".status.connected { background: #0f0; }\n"
".status.connecting { background: #ff0; animation: blink 0.5s infinite; }\n"
"@keyframes blink { 50% { opacity: 0.3; } }\n"
".conn-bar {\n"
"    background: #16213e; padding: 8px 16px; display: flex;\n"
"    align-items: center; gap: 8px; flex-shrink: 0;\n"
"    border-bottom: 1px solid #0f3460;\n"
"}\n"
".conn-bar label { font-size: 13px; color: #aaa; flex-shrink: 0; }\n"
".conn-bar input {\n"
"    background: #0f3460; border: 1px solid #533483; color: #eee;\n"
"    padding: 6px 8px; font-family: inherit; font-size: 13px;\n"
"    border-radius: 3px; min-width: 0; flex: 1;\n"
"}\n"
".conn-bar button {\n"
"    padding: 6px 16px; border: none; border-radius: 3px;\n"
"    font-family: inherit; font-size: 13px; cursor: pointer;\n"
"    white-space: nowrap; flex-shrink: 0;\n"
"}\n"
"#btnConnect { background: #0f3460; color: #eee; }\n"
"#btnDisconnect { background: #e94560; color: #eee; display: none; }\n"
".toolbar {\n"
"    background: #16213e; padding: 6px 16px; display: flex;\n"
"    align-items: center; gap: 10px; flex-shrink: 0;\n"
"    border-bottom: 1px solid #0f3460; font-size: 12px;\n"
"    flex-wrap: wrap;\n"
"}\n"
".toolbar label {\n"
"    color: #aaa; display: flex; align-items: center; gap: 4px;\n"
"    white-space: nowrap;\n"
"}\n"
".toolbar button {\n"
"    padding: 3px 10px; background: #0f3460; border: 1px solid #533483;\n"
"    color: #eee; border-radius: 3px; cursor: pointer; font-family: inherit;\n"
"    font-size: 12px;\n"
"}\n"
".toolbar .spacer { flex: 1; min-width: 0; }\n"
".ctrl-group {\n"
"    display: flex; align-items: center; gap: 6px;\n"
"}\n"
".ctrl-group .ctrl-sep {\n"
"    width: 1px; height: 14px; background: #533483;\n"
"}\n"
".ctrl-btn {\n"
"    padding: 3px 8px !important; font-size: 11px !important;\n"
"    background: #1a0a2e !important; border-color: #e94560 !important;\n"
"    color: #e94560 !important;\n"
"}\n"
".ctrl-btn:hover {\n"
"    background: #e94560 !important; color: #fff !important;\n"
"}\n"
".ctrl-btn:disabled {\n"
"    opacity: 0.3; cursor: not-allowed;\n"
"}\n"
".batt-display {\n"
"    display: inline-flex; align-items: center; gap: 4px;\n"
"    padding: 2px 8px; border-radius: 3px; font-size: 11px;\n"
"    background: #0a1a0a; border: 1px solid #1a3a1a; color: #4caf50;\n"
"}\n"
".batt-display .batt-icon {\n"
"    font-size: 13px;\n"
"}\n"
".led-group {\n"
"    display: flex; align-items: center; gap: 14px; font-size: 12px;\n"
"}\n"
".led-item { display: flex; align-items: center; gap: 4px; }\n"
".led {\n"
"    width: 10px; height: 10px; border-radius: 50%;\n"
"    background: #333; border: 1px solid #555;\n"
"    transition: background 0.15s;\n"
"}\n"
".led.active { background: #0f0; box-shadow: 0 0 6px #0f0; }\n"
".led-label { color: #aaa; }\n"
".pin-hint { color: #666; font-size: 11px; }\n"
".display-area {\n"
"    flex: 1; overflow-y: auto; padding: 8px 16px;\n"
"    font-size: 13px; line-height: 1.5; white-space: pre-wrap;\n"
"    word-break: break-all; background: #0d0d1a;\n"
"    -webkit-overflow-scrolling: touch;\n"
"}\n"
".display-area .rx { color: #00ffff; }\n"
".display-area .tx { color: #ffff00; }\n"
".display-area .sys { color: #888; font-style: italic; }\n"
".display-area .ctrl { color: #e94560; }\n"
".display-area .ts { color: #7f8c8d; }\n"
".send-bar {\n"
"    background: #16213e; padding: 8px 16px; display: flex;\n"
"    gap: 8px; flex-shrink: 0; border-top: 1px solid #0f3460;\n"
"}\n"
".toolbar-bottom {\n"
"    background: #16213e; padding: 6px 16px; display: flex;\n"
"    align-items: center; gap: 10px; flex-shrink: 0;\n"
"    border-top: 1px solid #0f3460; font-size: 12px;\n"
"}\n"
".toolbar-bottom button {\n"
"    padding: 3px 10px; background: #0f3460; border: 1px solid #533483;\n"
"    color: #eee; border-radius: 3px; cursor: pointer; font-family: inherit;\n"
"    font-size: 12px;\n"
"}\n"
".send-bar input {\n"
"    flex: 1; background: #0f3460; border: 1px solid #533483;\n"
"    color: #eee; padding: 8px 10px; font-family: inherit;\n"
"    font-size: 14px; border-radius: 3px; min-width: 0;\n"
"}\n"
".send-bar input:focus { outline: none; border-color: #e94560; }\n"
".send-bar button {\n"
"    padding: 8px 20px; background: #0f3460; border: 1px solid #533483;\n"
"    color: #eee; border-radius: 3px; cursor: pointer;\n"
"    font-family: inherit; font-size: 14px; white-space: nowrap; flex-shrink: 0;\n"
"}\n"
".line-end {\n"
"    display: flex; align-items: center; gap: 4px; font-size: 12px;\n"
"    color: #aaa; flex-shrink: 0;\n"
"}\n"
".line-end select {\n"
"    background: #0f3460; border: 1px solid #533483; color: #eee;\n"
"    padding: 4px; border-radius: 3px; font-family: inherit; font-size: 12px;\n"
"}\n"
"@media (max-width: 600px) {\n"
"    .header { padding: 8px 12px; gap: 8px; }\n"
"    .header h1 { font-size: 14px; }\n"
"    .header #statusText { font-size: 12px !important; }\n"
"    .conn-bar { padding: 8px 12px; flex-wrap: wrap; }\n"
"    .conn-bar label { display: none; }\n"
"    .conn-bar input { font-size: 16px; padding: 10px 12px; }\n"
"    .conn-bar button { padding: 10px 18px; font-size: 14px; }\n"
"    .toolbar { padding: 6px 12px; gap: 8px; }\n"
"    .toolbar label { font-size: 13px; }\n"
"    .toolbar label input[type=\"checkbox\"] {\n"
"        width: 18px; height: 18px; accent-color: #e94560;\n"
"    }\n"
"    .toolbar button { padding: 6px 12px; font-size: 13px; }\n"
"    .pin-hint { display: none; }\n"
"    #scrollHint { display: none; }\n"
"    .ctrl-group .ctrl-sep { display: none; }\n"
"    .ctrl-btn { padding: 4px 8px !important; font-size: 12px !important; }\n"
"    .display-area { padding: 6px 12px; font-size: 12px; line-height: 1.4; }\n"
"    .send-bar { padding: 8px 12px; gap: 6px; }\n"
"    .send-bar input { font-size: 16px; padding: 10px 12px; }\n"
"    .send-bar button { padding: 10px 16px; font-size: 14px; }\n"
"    .line-end select { font-size: 13px; padding: 6px; }\n"
"    .toolbar-bottom { padding: 6px 12px; }\n"
"    .toolbar-bottom label { font-size: 13px; }\n"
"    .toolbar-bottom label input[type=\"checkbox\"] {\n"
"        width: 18px; height: 18px; accent-color: #e94560;\n"
"    }\n"
"    .toolbar-bottom button { padding: 6px 12px; font-size: 13px; }\n"
"}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"header\">\n"
"    <h1>StickS3 Serial Bridge</h1>\n"
"    <span id=\"statusDot\" class=\"status\"></span>\n"
"    <span id=\"statusText\" style=\"font-size:13px;color:#aaa\">Disconnected</span>\n"
"</div>\n"
"<div class=\"conn-bar\">\n"
"    <label>Server:</label>\n"
"    <input id=\"inputHost\" type=\"text\" value=\"ws://%%IP%%:8080\"\n"
"           placeholder=\"ws://ip:port\" inputmode=\"url\">\n"
"    <button id=\"btnConnect\" onclick=\"doConnect()\">Connect</button>\n"
"    <button id=\"btnDisconnect\" onclick=\"doDisconnect()\">Disconnect</button>\n"
"</div>\n"
"<div class=\"toolbar\">\n"
"    <label><input type=\"checkbox\" id=\"chkHex\"> Hex</label>\n"
"    <label title=\"Middle-click display area to toggle\"><input type=\"checkbox\" id=\"chkAutoScroll\" checked onchange=\"syncScroll(this.checked)\"> Scroll</label>\n"
"    <span style=\"color:#666;font-size:11px\" id=\"scrollHint\">[mid-click toggle]</span>\n"
"    <label><input type=\"checkbox\" id=\"chkTimestamp\"> TS</label>\n"
"    <button onclick=\"clearDisplay()\">Clear</button>\n"
"    <div class=\"ctrl-group\" id=\"ctrlGroup\" style=\"display:none\">\n"
"        <span class=\"ctrl-sep\"></span>\n"
"        <button class=\"ctrl-btn\" onclick=\"sendCmd('REBOOT')\" id=\"btnReboot\">Reboot</button>\n"
"        <button class=\"ctrl-btn\" onclick=\"sendCmd('SCR_OFF')\" id=\"btnScrOff\">Screen Off</button>\n"
"        <button class=\"ctrl-btn\" onclick=\"sendCmd('SCR_ON')\" id=\"btnScrOn\">Screen On</button>\n"
"        <span class=\"batt-display\" id=\"battDisplay\" style=\"display:none\">"
"<span class=\"batt-icon\" id=\"battIcon\"></span>"
"<span id=\"battText\">--</span></span>\n"
"    </div>\n"
"    <span class=\"spacer\"></span>\n"
"    <div class=\"led-group\">\n"
"        <div class=\"led-item\">\n"
"            <div class=\"led\" id=\"ledRx\"></div>\n"
"            <span class=\"led-label\">RX</span>\n"
"        </div>\n"
"        <div class=\"led-item\">\n"
"            <div class=\"led\" id=\"ledTx\"></div>\n"
"            <span class=\"led-label\">TX</span>\n"
"        </div>\n"
"        <span class=\"pin-hint\">RX→G5, TX→G4</span>\n"
"    </div>\n"
"</div>\n"
"<div class=\"display-area\" id=\"display\"></div>\n"
"<div class=\"toolbar-bottom\">\n"
"    <label><input type=\"checkbox\" id=\"chkAutoScroll2\" checked onchange=\"syncScroll(this.checked)\"> Scroll</label>\n"
"    <button onclick=\"clearDisplay()\">Clear</button>\n"
"</div>\n"
"<div class=\"send-bar\">\n"
"    <input id=\"inputSend\" type=\"text\" placeholder=\"Send data...\"\n"
"           inputmode=\"text\" enterkeyhint=\"send\"\n"
"           onkeydown=\"if(event.key==='Enter')doSend()\">\n"
"    <div class=\"line-end\">\n"
"        <select id=\"selLineEnd\">\n"
"            <option value=\"none\">None</option>\n"
"            <option value=\"lf\" selected>LF</option>\n"
"            <option value=\"cr\">CR</option>\n"
"            <option value=\"crlf\">CRLF</option>\n"
"        </select>\n"
"    </div>\n"
"    <button onclick=\"doSend()\">Send</button>\n"
"</div>\n"
"<script>\n"
"let ws = null;\n"
"let rxChunks = [];\n"
"let rxActiveUntil = 0;\n"
"let txActiveUntil = 0;\n"
"let battTimer = null;\n"
"let battFailCount = 0;\n"
"function flashRx() {\n"
"    rxActiveUntil = Date.now() + 1000;\n"
"    document.getElementById('ledRx').classList.add('active');\n"
"}\n"
"function flashTx() {\n"
"    txActiveUntil = Date.now() + 1000;\n"
"    document.getElementById('ledTx').classList.add('active');\n"
"}\n"
"function updateLeds() {\n"
"    const now = Date.now();\n"
"    if (now > rxActiveUntil) document.getElementById('ledRx').classList.remove('active');\n"
"    if (now > txActiveUntil) document.getElementById('ledTx').classList.remove('active');\n"
"}\n"
"function formatTimestamp() {\n"
"    const d = new Date();\n"
"    const h = String(d.getHours()).padStart(2, '0');\n"
"    const m = String(d.getMinutes()).padStart(2, '0');\n"
"    const s = String(d.getSeconds()).padStart(2, '0');\n"
"    const ms = String(d.getMilliseconds()).padStart(3, '0');\n"
"    return h + ':' + m + ':' + s + '.' + ms;\n"
"}\n"
"function log(cls, text) {\n"
"    const el = document.getElementById('display');\n"
"    if (document.getElementById('chkTimestamp').checked) {\n"
"        const tsSpan = document.createElement('span');\n"
"        tsSpan.className = 'ts';\n"
"        tsSpan.textContent = '[' + formatTimestamp() + '] ';\n"
"        el.appendChild(tsSpan);\n"
"    }\n"
"    const span = document.createElement('span');\n"
"    span.className = cls;\n"
"    span.textContent = text + '\\n';\n"
"    el.appendChild(span);\n"
"    while (el.children.length > 5000) el.removeChild(el.firstChild);\n"
"    if (document.getElementById('chkAutoScroll').checked) {\n"
"        el.scrollTop = el.scrollHeight;\n"
"    }\n"
"}\n"
"function setStatus(state, text) {\n"
"    document.getElementById('statusDot').className = 'status ' + state;\n"
"    document.getElementById('statusText').textContent = text;\n"
"    document.getElementById('ctrlGroup').style.display = state === 'connected' ? 'flex' : 'none';\n"
"}\n"
"function formatHex(buf) {\n"
"    const bytes = new Uint8Array(buf);\n"
"    let hex = '';\n"
"    for (let i = 0; i < bytes.length; i++) {\n"
"        hex += bytes[i].toString(16).padStart(2, '0') + ' ';\n"
"    }\n"
"    return hex.trim();\n"
"}\n"
"function flushRxBuffer() {\n"
"    if (rxChunks.length === 0) return;\n"
"    const totalLen = rxChunks.reduce((s, c) => s + c.byteLength, 0);\n"
"    const merged = new Uint8Array(totalLen);\n"
"    let off = 0;\n"
"    for (const chunk of rxChunks) {\n"
"        merged.set(new Uint8Array(chunk), off);\n"
"        off += chunk.byteLength;\n"
"    }\n"
"    rxChunks = [];\n"
"    if (document.getElementById('chkHex').checked) {\n"
"        log('rx', 'RX: ' + formatHex(merged.buffer));\n"
"    } else {\n"
"        log('rx', 'RX: ' + new TextDecoder('utf-8', {fatal: false}).decode(merged.buffer));\n"
"    }\n"
"}\n"
"function updateBattDisplay(mv) {\n"
"    const el = document.getElementById('battDisplay');\n"
"    const txt = document.getElementById('battText');\n"
"    const icon = document.getElementById('battIcon');\n"
"    el.style.display = 'inline-flex';\n"
"    const v = (mv / 1000).toFixed(2);\n"
"    txt.textContent = v + 'V';\n"
"    if (mv >= 4000) { icon.textContent = '\\uD83D\\uDD0C'; el.style.color = '#4caf50'; }\n"
"    else if (mv >= 3700) { icon.textContent = '\\uD83D\\uDD0B'; el.style.color = '#4caf50'; }\n"
"    else if (mv >= 3500) { icon.textContent = '\\uD83D\\uDD0B'; el.style.color = '#ff9800'; }\n"
"    else { icon.textContent = '\\uD83D\\uDD0B'; el.style.color = '#f44336'; }\n"
"}\n"
"function startBattPolling() {\n"
"    stopBattPolling();\n"
"    battFailCount = 0;\n"
"    sendCmd('BATT');\n"
"    battTimer = setInterval(function() {\n"
"        if (!ws || ws.readyState !== WebSocket.OPEN) { stopBattPolling(); return; }\n"
"        sendCmd('BATT');\n"
"    }, 10000);\n"
"}\n"
"function stopBattPolling() {\n"
"    if (battTimer) { clearInterval(battTimer); battTimer = null; }\n"
"    battFailCount = 0;\n"
"    var el = document.getElementById('battDisplay');\n"
"    if (el) el.style.display = 'none';\n"
"}\n"
"function handleBattResp(resp) {\n"
"    var parts = resp.split(' ');\n"
"    if (parts.length >= 2 && parts[0] === 'BATT') {\n"
"        var mv = parseInt(parts[1], 10);\n"
"        if (!isNaN(mv) && mv > 0) {\n"
"            battFailCount = 0;\n"
"            updateBattDisplay(mv);\n"
"            return;\n"
"        }\n"
"    }\n"
"    battFailCount++;\n"
"    if (battFailCount >= 2) {\n"
"        log('sys', 'Battery query failed, connection may be lost');\n"
"        doDisconnect();\n"
"    }\n"
"}\n"
"function doConnect() {\n"
"    const url = document.getElementById('inputHost').value.trim();\n"
"    if (!url) return;\n"
"    if (ws) doDisconnect();\n"
"    setStatus('connecting', 'Connecting...');\n"
"    log('sys', 'Connecting to ' + url + '...');\n"
"    try {\n"
"        ws = new WebSocket(url);\n"
"        ws.binaryType = 'arraybuffer';\n"
"    } catch(e) {\n"
"        setStatus('', 'Disconnected');\n"
"        log('sys', 'Invalid URL: ' + e.message);\n"
"        return;\n"
"    }\n"
"    ws.onopen = function() {\n"
"        setStatus('connected', 'Connected');\n"
"        log('sys', 'Connected to ' + url);\n"
"        document.getElementById('btnConnect').style.display = 'none';\n"
"        document.getElementById('btnDisconnect').style.display = 'inline-block';\n"
"        startBattPolling();\n"
"    };\n"
"    ws.onmessage = function(evt) {\n"
"        if (evt.data instanceof ArrayBuffer) {\n"
"            const bytes = new Uint8Array(evt.data);\n"
"            if (bytes.length > 1 && bytes[0] === 0x01) {\n"
"                const resp = new TextDecoder().decode(evt.data.slice(1));\n"
"                if (resp.startsWith('BATT ')) { handleBattResp(resp); }\n"
"                else { log('ctrl', 'CMD: ' + resp); }\n"
"            } else {\n"
"                flashRx(); rxChunks.push(evt.data);\n"
"            }\n"
"        } else {\n"
"            const txt = evt.data;\n"
"            if (txt.charCodeAt(0) === 1) {\n"
"                const resp = txt.slice(1);\n"
"                if (resp.startsWith('BATT ')) { handleBattResp(resp); }\n"
"                else { log('ctrl', 'CMD: ' + resp); }\n"
"            }\n"
"        }\n"
"    };\n"
"    ws.onclose = function() {\n"
"        stopBattPolling();\n"
"        setStatus('', 'Disconnected');\n"
"        log('sys', 'Connection closed');\n"
"        ws = null;\n"
"        document.getElementById('btnConnect').style.display = '';\n"
"        document.getElementById('btnDisconnect').style.display = 'none';\n"
"    };\n"
"    ws.onerror = function() { log('sys', 'WebSocket error'); };\n"
"}\n"
"function doDisconnect() {\n"
"    if (ws) { ws.close(); ws = null; }\n"
"    setStatus('', 'Disconnected');\n"
"    document.getElementById('btnConnect').style.display = '';\n"
"    document.getElementById('btnDisconnect').style.display = 'none';\n"
"}\n"
"function doSend() {\n"
"    if (!ws || ws.readyState !== WebSocket.OPEN) { log('sys', 'Not connected'); return; }\n"
"    const input = document.getElementById('inputSend');\n"
"    let data = input.value;\n"
"    if (!data) return;\n"
"    const le = document.getElementById('selLineEnd').value;\n"
"    if (le === 'lf') data += '\\n';\n"
"    else if (le === 'cr') data += '\\r';\n"
"    else if (le === 'crlf') data += '\\r\\n';\n"
"    ws.send(new TextEncoder().encode(data).buffer);\n"
"    flashTx();\n"
"    log('tx', 'TX: ' + data);\n"
"    input.value = '';\n"
"}\n"
"function sendCmd(cmd) {\n"
"    if (!ws || ws.readyState !== WebSocket.OPEN) { log('sys', 'Not connected'); return; }\n"
"    ws.send(String.fromCharCode(1) + cmd);\n"
"    log('ctrl', 'CMD: ' + cmd);\n"
"}\n"
"function clearDisplay() {\n"
"    document.getElementById('display').innerHTML = '';\n"
"}\n"
"function syncScroll(checked) {\n"
"    document.getElementById('chkAutoScroll').checked = checked;\n"
"    document.getElementById('chkAutoScroll2').checked = checked;\n"
"}\n"
"// Middle mouse button toggles scroll\n"
"document.getElementById('display').addEventListener('mousedown', function(e) {\n"
"    if (e.button === 1) {\n"
"        e.preventDefault();\n"
"        var chk = document.getElementById('chkAutoScroll');\n"
"        chk.checked = !chk.checked;\n"
"        syncScroll(chk.checked);\n"
"    }\n"
"});\n"
"\n"
"setInterval(function() { flushRxBuffer(); updateLeds(); }, 50);\n"
"if (window.location.hostname && window.location.hostname !== 'localhost') {\n"
"    document.getElementById('inputHost').value = 'ws://' + window.location.hostname + ':8080';\n"
"}\n"
"doConnect();\n"
"</script>\n"
"</body>\n"
"</html>";

// ─── Busy page (shown when WS client is already connected) ───

static const char BUSY_HTML[] =
"<!DOCTYPE html><html><head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>StickS3 - Device Busy</title>"
"<style>"
"body{font-family:Consolas,Monaco,monospace;background:#1a1a2e;color:#eee;"
"height:100vh;display:flex;align-items:center;justify-content:center;flex-direction:column}"
".box{background:#16213e;border:1px solid #0f3460;border-radius:8px;padding:40px;text-align:center;max-width:400px}"
"h2{color:#e94560;margin-bottom:16px}"
"p{color:#aaa;margin-bottom:20px;line-height:1.6}"
"button{padding:10px 30px;background:#0f3460;border:1px solid #533483;color:#eee;"
"border-radius:4px;font-size:14px;cursor:pointer;font-family:inherit}"
"button:hover{background:#e94560}"
"</style></head><body>"
"<div class=\"box\">"
"<h2>Device Busy</h2>"
"<p>A WebSocket client is currently connected to this device. "
"Please disconnect the existing connection first, then try again.</p>"
"</div></body></html>";

// ─── Helpers ───

static void send_http_response(int client, const char *status, const char *content_type,
                                const char *body, int body_len) {
    char header[256];
    int hdr_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        status, content_type, body_len);
    send(client, header, hdr_len, 0);
    // Send body in chunks (may be large)
    int sent = 0;
    while (sent < body_len) {
        int n = send(client, body + sent, body_len - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}

// ─── Client handler ───

static void handle_client(int client) {
    char buf[512];
    int len = recv(client, buf, sizeof(buf) - 1, 0);
    if (len <= 0) {
        close(client);
        return;
    }
    buf[len] = '\0';

    // Only handle GET requests
    if (strncmp(buf, "GET ", 4) != 0) {
        send_http_response(client, "405 Method Not Allowed", "text/plain", "Method Not Allowed", 18);
        close(client);
        return;
    }

    if (s_ws_connected) {
        // WS client is connected — serve busy page
        ESP_LOGI(TAG, "WS busy, serving busy page");
        send_http_response(client, "200 OK", "text/html", BUSY_HTML, strlen(BUSY_HTML));
    } else {
        // Serve pc_tool.html with IP substitution
        char ip[32] = "0.0.0.0";
        sticks3_wifi_get_ip(ip, sizeof(ip));

        // Build response with %%IP%% replaced by actual IP
        size_t html_len = strlen(TOOL_HTML);
        size_t ip_len = strlen(ip);
        // %%IP%% is 6 chars, replaced by ip_len chars
        size_t resp_len = html_len - 6 + ip_len;
        char *resp = malloc(resp_len + 1);
        if (!resp) {
            send_http_response(client, "500 Internal Server Error", "text/plain", "Out of memory", 13);
            close(client);
            return;
        }

        // Find %%IP%% placeholder and build response
        const char *placeholder = strstr(TOOL_HTML, "%%IP%%");
        if (placeholder) {
            size_t before = placeholder - TOOL_HTML;
            memcpy(resp, TOOL_HTML, before);
            memcpy(resp + before, ip, ip_len);
            size_t after_offset = before + 6; // skip %%IP%%
            size_t after_len = html_len - after_offset;
            memcpy(resp + before + ip_len, TOOL_HTML + after_offset, after_len);
            resp[resp_len] = '\0';
        } else {
            // No placeholder found, serve as-is
            memcpy(resp, TOOL_HTML, html_len);
            resp_len = html_len;
            resp[resp_len] = '\0';
        }

        ESP_LOGI(TAG, "Serving pc_tool.html (ip=%s, %d bytes)", ip, (int)resp_len);
        send_http_response(client, "200 OK", "text/html", resp, (int)resp_len);
        free(resp);
    }
    close(client);
}

// ─── Server task ───

static void http_server_task(void *arg) {
    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_listen_sock < 0) {
        ESP_LOGE(TAG, "Socket create failed: errno=%d", errno);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: errno=%d", errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (listen(s_listen_sock, 3) < 0) {
        ESP_LOGE(TAG, "Listen failed: errno=%d", errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "HTTP server listening on port 80");

    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client = accept(s_listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client < 0) {
            if (s_running) ESP_LOGE(TAG, "Accept failed: errno=%d", errno);
            continue;
        }

        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_client(client);
    }

    if (s_listen_sock >= 0) {
        close(s_listen_sock);
        s_listen_sock = -1;
    }
    ESP_LOGI(TAG, "HTTP server stopped");
    s_http_task = NULL;
    vTaskDelete(NULL);
}

// ─── Public API ───

esp_err_t sticks3_http_server_start(void) {
    if (s_running) return ESP_OK;
    s_running = true;
    xTaskCreate(http_server_task, "http_srv", 8192, NULL, 4, &s_http_task);
    ESP_LOGI(TAG, "HTTP server starting on port 80");
    return ESP_OK;
}

esp_err_t sticks3_http_server_stop(void) {
    if (!s_running) return ESP_OK;
    s_running = false;
    // Close listen socket to unblock accept()
    if (s_listen_sock >= 0) {
        close(s_listen_sock);
        s_listen_sock = -1;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    return ESP_OK;
}

void sticks3_http_server_set_ws_connected(bool connected) {
    s_ws_connected = connected;
}
