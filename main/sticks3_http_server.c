#include "sticks3_http_server.h"
#include "sticks3_wifi.h"
#include "sticks3_power.h"
#include "sticks3_nvs.h"
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
static sticks3_http_get_baud_cb_t s_get_baud_cb = NULL;
static sticks3_http_set_baud_cb_t s_set_baud_cb = NULL;

#define MACROS_JSON_MAX 2048

// ─── Embedded HTML (placeholder %%IP%% will be replaced with device IP) ───

static const char TOOL_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no\">\n"
"<title>StickS3 Serial Bridge</title>\n"
"<link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/npm/@xterm/xterm@5.5.0/css/xterm.min.css\">\n"
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
".toolbar button {\n"
"    padding: 3px 10px; background: #0f3460; border: 1px solid #533483;\n"
"    color: #eee; border-radius: 3px; cursor: pointer; font-family: inherit;\n"
"    font-size: 12px;\n"
"}\n"
".toolbar label {\n"
"    color: #aaa; display: inline-flex; align-items: center; gap: 4px;\n"
"    white-space: nowrap;\n"
"}\n"
".toolbar select {\n"
"    background: #0f3460; border: 1px solid #533483; color: #eee;\n"
"    border-radius: 3px; padding: 3px 6px; font-family: inherit; font-size: 12px;\n"
"}\n"
".toolbar .spacer { flex: 1; min-width: 0; }\n"
".footbar {\n"
"    background: #16213e; padding: 6px 16px; display: grid;\n"
"    grid-template-columns: auto 1fr; align-items: start; gap: 8px; flex-shrink: 0;\n"
"    border-top: 1px solid #0f3460; font-size: 12px;\n"
"}\n"
".footbar label {\n"
"    color: #aaa; display: inline-flex; align-items: center; gap: 4px;\n"
"    white-space: nowrap;\n"
"}\n"
".footbar button {\n"
"    padding: 4px 12px; background: #0f3460; border: 1px solid #533483;\n"
"    color: #eee; border-radius: 3px; cursor: pointer; font-family: inherit;\n"
"    font-size: 12px;\n"
"}\n"
".macro-grid {\n"
"    display: flex; flex-wrap: wrap; gap: 6px;\n"
"}\n"
".macro-add {\n"
"    color: #4caf50 !important;\n"
"}\n"
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
"#terminal { flex: 1; min-height: 0; }\n"
"#terminal .xterm { height: 100%; }\n"
"#terminal .xterm-viewport { height: 100% !important; }\n"
"@media (max-width: 600px) {\n"
"    .header { padding: 8px 12px; gap: 8px; }\n"
"    .header h1 { font-size: 14px; }\n"
"    .header #statusText { font-size: 12px !important; }\n"
"    .conn-bar { padding: 8px 12px; flex-wrap: wrap; }\n"
"    .conn-bar label { display: none; }\n"
"    .conn-bar input { font-size: 16px; padding: 10px 12px; }\n"
"    .conn-bar button { padding: 10px 18px; font-size: 14px; }\n"
"    .toolbar { padding: 6px 12px; gap: 8px; }\n"
"    .toolbar button { padding: 6px 12px; font-size: 13px; }\n"
"    .footbar { padding: 6px 12px; grid-template-columns: 1fr; gap: 6px; }\n"
"    .pin-hint { display: none; }\n"
"    .ctrl-group .ctrl-sep { display: none; }\n"
"    .ctrl-btn { padding: 4px 8px !important; font-size: 12px !important; }\n"
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
"    <button onclick=\"clearTerminal()\">Clear</button>\n"
"    <label>Baud\n"
"        <select id=\"baudSelect\" onchange=\"setBaud(this.value)\">\n"
"            <option value=\"115200\">115200</option>\n"
"            <option value=\"230400\">230400</option>\n"
"            <option value=\"460800\">460800</option>\n"
"            <option value=\"921600\">921600</option>\n"
"            <option value=\"1000000\">1000000</option>\n"
"            <option value=\"1500000\">1500000</option>\n"
"            <option value=\"2000000\">2000000</option>\n"
"        </select>\n"
"    </label>\n"
"    <label><input type=\"checkbox\" id=\"chkTimestamp\"> TS</label>\n"
"    <label><input type=\"checkbox\" id=\"chkAutoScroll\" checked onchange=\"setAutoScroll(this.checked)\"> Scroll</label>\n"
"    <span class=\"batt-display\" id=\"battDisplay\"><span id=\"battText\">--</span></span>\n"
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
"<div id=\"terminal\"></div>\n"
"<div class=\"footbar\">\n"
"    <label>Macro</label>\n"
"    <div class=\"macro-grid\" id=\"macroButtons\"></div>\n"
"</div>\n"
"<script src=\"https://cdn.jsdelivr.net/npm/@xterm/xterm@5.5.0/lib/xterm.min.js\"></script>\n"
"<script src=\"https://cdn.jsdelivr.net/npm/@xterm/addon-fit@0.10.0/lib/addon-fit.min.js\"></script>\n"
"<script>\n"
"let ws = null;\n"
"let rxActiveUntil = 0;\n"
"let txActiveUntil = 0;\n"
"let macros = [];\n"
"let autoScroll = true;\n"
"const defaultMacros = [\n"
"    { name: 'kmsg off', data: 'echo 0 > /proc/sys/kernel/printk\\\\r' },\n"
"    { name: 'reboot loader', data: 'reboot loader\\\\r' },\n"
"    { name: 'dmesg', data: 'dmesg -w\\\\r' },\n"
"    { name: 'ifconfig', data: 'ifconfig\\\\r' },\n"
"];\n"
"const term = new Terminal({\n"
"    cursorBlink: true,\n"
"    fontSize: 14,\n"
"    fontFamily: \"'Consolas', 'Monaco', 'Courier New', monospace\",\n"
"    theme: {\n"
"        background: '#0d0d1a',\n"
"        foreground: '#eeeeee',\n"
"        cursor: '#e94560',\n"
"        selectionBackground: '#264f78',\n"
"    },\n"
"    scrollback: 5000,\n"
"});\n"
"const fitAddon = new FitAddon.FitAddon();\n"
"term.loadAddon(fitAddon);\n"
"term.open(document.getElementById('terminal'));\n"
"function fitTerminal() {\n"
"    try {\n"
"        fitAddon.fit();\n"
"    } catch (e) {}\n"
"}\n"
"fitTerminal();\n"
"window.addEventListener('resize', fitTerminal);\n"
"setTimeout(fitTerminal, 0);\n"
"setTimeout(fitTerminal, 250);\n"
"term.onData(function(data) {\n"
"    if (!ws || ws.readyState !== WebSocket.OPEN) return;\n"
"    ws.send(new TextEncoder().encode(data).buffer);\n"
"    flashTx();\n"
"});\n"
"term.write('\\x1b[36mStickS3 Serial Bridge\\x1b[0m\\r\\n');\n"
"function focusTerminal() {\n"
"    term.focus();\n"
"}\n"
"function clearTerminal() {\n"
"    term.clear();\n"
"    focusTerminal();\n"
"}\n"
"function setAutoScroll(enabled) {\n"
"    autoScroll = enabled;\n"
"    const chk = document.getElementById('chkAutoScroll');\n"
"    if (chk) chk.checked = autoScroll;\n"
"    if (autoScroll) term.scrollToBottom();\n"
"}\n"
"document.getElementById('terminal').addEventListener('mousedown', function(e) {\n"
"    if (e.button === 1) {\n"
"        e.preventDefault();\n"
"        setAutoScroll(!autoScroll);\n"
"        term.write('\\r\\n\\x1b[36mAuto scroll ' + (autoScroll ? 'ON' : 'OFF') + '\\x1b[0m\\r\\n');\n"
"        focusTerminal();\n"
"    }\n"
"});\n"
"function flashRx() {\n"
"    rxActiveUntil = Date.now() + 1000;\n"
"    document.getElementById('ledRx').classList.add('active');\n"
"}\n"
"function flashTx() {\n"
"    txActiveUntil = Date.now() + 1000;\n"
"    document.getElementById('ledTx').classList.add('active');\n"
"}\n"
"setInterval(function() {\n"
"    const now = Date.now();\n"
"    if (now > rxActiveUntil) document.getElementById('ledRx').classList.remove('active');\n"
"    if (now > txActiveUntil) document.getElementById('ledTx').classList.remove('active');\n"
"}, 50);\n"
"function setStatus(state, text) {\n"
"    document.getElementById('statusDot').className = 'status ' + state;\n"
"    document.getElementById('statusText').textContent = text;\n"
"}\n"
"function updateBattDisplay(mv) {\n"
"    const el = document.getElementById('battDisplay');\n"
"    const txt = document.getElementById('battText');\n"
"    const icon = document.getElementById('battIcon');\n"
"    el.style.display = 'inline-flex';\n"
"    const v = (mv / 1000).toFixed(2);\n"
"    txt.textContent = v + 'V';\n"
"    if (mv >= 4000) { icon.textContent = '\\u{1F50C}'; el.style.color = '#4caf50'; }\n"
"    else if (mv >= 3700) { icon.textContent = '\\u{1F50B}'; el.style.color = '#4caf50'; }\n"
"    else if (mv >= 3500) { icon.textContent = '\\u{1F50B}'; el.style.color = '#ff9800'; }\n"
"    else { icon.textContent = '\\u{1F50B}'; el.style.color = '#f44336'; }\n"
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
"        term.write('\\r\\n\\x1b[31mBattery query failed, connection may be lost\\x1b[0m\\r\\n');\n"
"        doDisconnect();\n"
"    }\n"
"}\n"
"function sendCmd(cmd) {\n"
"    if (!ws || ws.readyState !== WebSocket.OPEN) return;\n"
"    ws.send(String.fromCharCode(1) + cmd);\n"
"}\n"
"function apiBase() {\n"
"    return window.location.origin && window.location.protocol.startsWith('http')\n"
"        ? window.location.origin\n"
"        : 'http://' + document.getElementById('inputHost').value.replace(/^ws:\\/\\//, '').replace(/:8080.*$/, '');\n"
"}\n"
"function updateBattDisplay(mv, charging) {\n"
"    const el = document.getElementById('battDisplay');\n"
"    const txt = document.getElementById('battText');\n"
"    el.style.display = 'inline-flex';\n"
"    txt.textContent = (charging ? 'CHG ' : '') + (mv / 1000).toFixed(2) + 'V';\n"
"    if (mv >= 3700) el.style.color = '#4caf50';\n"
"    else if (mv >= 3500) el.style.color = '#ff9800';\n"
"    else el.style.color = '#f44336';\n"
"}\n"
"async function refreshStatus() {\n"
"    try {\n"
"        const resp = await fetch(apiBase() + '/api/status', { cache: 'no-store' });\n"
"        const data = await resp.json();\n"
"        if (!data.ok) return;\n"
"        if (data.baud) document.getElementById('baudSelect').value = String(data.baud);\n"
"        if (data.batt_ok) updateBattDisplay(data.batt_mv, data.charging);\n"
"    } catch (e) {}\n"
"}\n"
"async function setBaud(value) {\n"
"    try {\n"
"        const resp = await fetch(apiBase() + '/api/baud?value=' + encodeURIComponent(value), { cache: 'no-store' });\n"
"        const data = await resp.json();\n"
"        if (!data.ok) throw new Error(data.error || 'failed');\n"
"        document.getElementById('baudSelect').value = String(data.baud);\n"
"        term.write('\\r\\n\\x1b[36mBaud set to ' + data.baud + '\\x1b[0m\\r\\n');\n"
"    } catch (e) {\n"
"        term.write('\\r\\n\\x1b[31mSet baud failed\\x1b[0m\\r\\n');\n"
"        refreshStatus();\n"
"    }\n"
"}\n"
"function renderMacros() {\n"
"    const box = document.getElementById('macroButtons');\n"
"    box.innerHTML = '';\n"
"    if (macros.length === 0) {\n"
"        macros = defaultMacros.slice();\n"
"    }\n"
"    macros.forEach(function(m, idx) {\n"
"        const btn = document.createElement('button');\n"
"        btn.textContent = m.name || ('Macro ' + (idx + 1));\n"
"        btn.title = 'Left click: run, right click: edit';\n"
"        btn.onclick = function() { runMacro(idx); };\n"
"        btn.oncontextmenu = function(e) {\n"
"            e.preventDefault();\n"
"            editMacro(idx);\n"
"        };\n"
"        box.appendChild(btn);\n"
"    });\n"
"    const add = document.createElement('button');\n"
"    add.className = 'macro-add';\n"
"    add.textContent = '+';\n"
"    add.title = 'Add macro';\n"
"    add.onclick = function() { editMacro(-1); };\n"
"    box.appendChild(add);\n"
"}\n"
"async function loadMacros() {\n"
"    try {\n"
"        const resp = await fetch(apiBase() + '/api/macros', { cache: 'no-store' });\n"
"        const data = await resp.json();\n"
"        macros = Array.isArray(data.macros) ? data.macros : [];\n"
"        renderMacros();\n"
"    } catch (e) {\n"
"        macros = [];\n"
"        renderMacros();\n"
"    }\n"
"}\n"
"async function saveMacros() {\n"
"    const resp = await fetch(apiBase() + '/api/macros', {\n"
"        method: 'POST',\n"
"        headers: { 'Content-Type': 'application/json' },\n"
"        body: JSON.stringify(macros)\n"
"    });\n"
"    const data = await resp.json();\n"
"    if (!data.ok) throw new Error(data.error || 'save_failed');\n"
"}\n"
"function encodeMacroData(text) {\n"
"    return text\n"
"        .replace(/\\\\/g, '\\\\\\\\')\n"
"        .replace(/\\r/g, '\\\\r')\n"
"        .replace(/\\n/g, '\\\\n')\n"
"        .replace(/\\t/g, '\\\\t');\n"
"}\n"
"function decodeMacroData(text) {\n"
"    return text.replace(/\\\\x([0-9a-fA-F]{2})|\\\\r|\\\\n|\\\\t|\\\\\\\\/g, function(m, hex) {\n"
"        if (hex) return String.fromCharCode(parseInt(hex, 16));\n"
"        if (m === '\\\\r') return '\\r';\n"
"        if (m === '\\\\n') return '\\n';\n"
"        if (m === '\\\\t') return '\\t';\n"
"        return '\\\\';\n"
"    });\n"
"}\n"
"function runMacro(index) {\n"
"    const macro = macros[index];\n"
"    if (!macro || !ws || ws.readyState !== WebSocket.OPEN) return;\n"
"    const data = decodeMacroData(macro.data || '');\n"
"    ws.send(new TextEncoder().encode(data).buffer);\n"
"    flashTx();\n"
"}\n"
"async function editMacro(index) {\n"
"    const old = index >= 0 ? macros[index] : { name: '', data: '' };\n"
"    const text = prompt('Edit macro: name=command. Escapes: \\\\r \\\\n \\\\t \\\\x03',\n"
"        (old.name || '') + '=' + encodeMacroData(old.data || ''));\n"
"    if (text === null) return;\n"
"    const line = text.trim();\n"
"    if (!line && index >= 0) {\n"
"        macros.splice(index, 1);\n"
"    } else {\n"
"        const eq = line.indexOf('=');\n"
"        if (eq <= 0) return;\n"
"        const name = line.slice(0, eq).trim().slice(0, 24);\n"
"        const data = line.slice(eq + 1).slice(0, 160);\n"
"        if (!name) return;\n"
"        if (index >= 0) macros[index] = { name: name, data: data };\n"
"        else macros.push({ name: name, data: data });\n"
"    }\n"
"    macros = macros.slice(0, 24);\n"
"    renderMacros();\n"
"    try {\n"
"        await saveMacros();\n"
"        term.write('\\r\\n\\x1b[36mMacros saved\\x1b[0m\\r\\n');\n"
"    } catch (e) {\n"
"        term.write('\\r\\n\\x1b[31mSave macros failed\\x1b[0m\\r\\n');\n"
"        loadMacros();\n"
"    }\n"
"}\n"
"async function editMacros() {\n"
"    const current = macros.map(function(m) {\n"
"        return (m.name || '') + '=' + encodeMacroData(m.data || '');\n"
"    }).join('\\n');\n"
"    const text = prompt('Edit macros, one per line: name=command. Escapes: \\\\r \\\\n \\\\t \\\\x03', current);\n"
"    if (text === null) return;\n"
"    const next = [];\n"
"    text.split(/\\r?\\n/).forEach(function(line) {\n"
"        line = line.trim();\n"
"        if (!line) return;\n"
"        const eq = line.indexOf('=');\n"
"        if (eq <= 0) return;\n"
"        const name = line.slice(0, eq).trim().slice(0, 24);\n"
"        const data = line.slice(eq + 1).slice(0, 160);\n"
"        if (name) next.push({ name: name, data: data });\n"
"    });\n"
"    macros = next.slice(0, 24);\n"
"    renderMacros();\n"
"    try {\n"
"        await saveMacros();\n"
"        term.write('\\r\\n\\x1b[36mMacros saved\\x1b[0m\\r\\n');\n"
"    } catch (e) {\n"
"        term.write('\\r\\n\\x1b[31mSave macros failed\\x1b[0m\\r\\n');\n"
"        loadMacros();\n"
"    }\n"
"}\n"
"const rxDecoder = new TextDecoder();\n"
"let rxLineStart = true;\n"
"function timestampText() {\n"
"    const d = new Date();\n"
"    return '[' +\n"
"        String(d.getHours()).padStart(2, '0') + ':' +\n"
"        String(d.getMinutes()).padStart(2, '0') + ':' +\n"
"        String(d.getSeconds()).padStart(2, '0') + '.' +\n"
"        String(d.getMilliseconds()).padStart(3, '0') + '] ';\n"
"}\n"
"function writeRxData(bytesOrText) {\n"
"    if (!document.getElementById('chkTimestamp').checked) {\n"
"        term.write(bytesOrText);\n"
"        if (autoScroll) term.scrollToBottom();\n"
"        return;\n"
"    }\n"
"    const text = typeof bytesOrText === 'string' ? bytesOrText : rxDecoder.decode(bytesOrText, { stream: true });\n"
"    let out = '';\n"
"    for (let i = 0; i < text.length; i++) {\n"
"        if (rxLineStart && text[i] !== '\\r' && text[i] !== '\\n') {\n"
"            out += timestampText();\n"
"            rxLineStart = false;\n"
"        }\n"
"        out += text[i];\n"
"        if (text[i] === '\\n') rxLineStart = true;\n"
"    }\n"
"    term.write(out);\n"
"    if (autoScroll) term.scrollToBottom();\n"
"}\n"
"function doConnect() {\n"
"    const url = document.getElementById('inputHost').value.trim();\n"
"    if (!url) return;\n"
"    if (ws) doDisconnect();\n"
"    setStatus('connecting', 'Connecting...');\n"
"    term.write('\\x1b[33mConnecting to ' + url + '...\\x1b[0m\\r\\n');\n"
"    try {\n"
"        ws = new WebSocket(url);\n"
"        ws.binaryType = 'arraybuffer';\n"
"    } catch(e) {\n"
"        setStatus('', 'Disconnected');\n"
"        term.write('\\x1b[31mInvalid URL: ' + e.message + '\\x1b[0m\\r\\n');\n"
"        return;\n"
"    }\n"
"    ws.onopen = function() {\n"
"        setStatus('connected', 'Connected');\n"
"        term.write('\\x1b[32mConnected\\x1b[0m\\r\\n');\n"
"        document.getElementById('btnConnect').style.display = 'none';\n"
"        document.getElementById('btnDisconnect').style.display = 'inline-block';\n"
"        fitTerminal();\n"
"        refreshStatus();\n"
"    };\n"
"    ws.onmessage = function(evt) {\n"
"        if (evt.data instanceof ArrayBuffer) {\n"
"            const bytes = new Uint8Array(evt.data);\n"
"            if (false) {\n"
"                const resp = new TextDecoder().decode(evt.data.slice(1));\n"
"                if (resp.startsWith('BATT ')) { handleBattResp(resp); }\n"
"                else { term.write('\\x1b[35m[CMD] ' + resp + '\\x1b[0m\\r\\n'); }\n"
"            } else {\n"
"                flashRx();\n"
"                writeRxData(bytes);\n"
"            }\n"
"        } else {\n"
"            const txt = evt.data;\n"
"            if (false) {\n"
"                const resp = txt.slice(1);\n"
"                if (resp.startsWith('BATT ')) { handleBattResp(resp); }\n"
"                else { term.write('\\x1b[35m[CMD] ' + resp + '\\x1b[0m\\r\\n'); }\n"
"            } else {\n"
"                flashRx();\n"
"                writeRxData(txt);\n"
"            }\n"
"        }\n"
"    };\n"
"    ws.onclose = function() {\n"
"        setStatus('', 'Disconnected');\n"
"        term.write('\\x1b[31mConnection closed\\x1b[0m\\r\\n');\n"
"        ws = null;\n"
"        document.getElementById('btnConnect').style.display = '';\n"
"        document.getElementById('btnDisconnect').style.display = 'none';\n"
"        fitTerminal();\n"
"    };\n"
"    ws.onerror = function() { term.write('\\x1b[31mWebSocket error\\x1b[0m\\r\\n'); };\n"
"}\n"
"function doDisconnect() {\n"
"    if (ws) { ws.close(); ws = null; }\n"
"    setStatus('', 'Disconnected');\n"
"    document.getElementById('btnConnect').style.display = '';\n"
"    document.getElementById('btnDisconnect').style.display = 'none';\n"
"}\n"
"if (window.location.hostname && window.location.hostname !== 'localhost') {\n"
"    document.getElementById('inputHost').value = 'ws://' + window.location.hostname + ':8080';\n"
"}\n"
"refreshStatus();\n"
"loadMacros();\n"
"setInterval(refreshStatus, 10000);\n"
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
    int hdr_sent = 0;
    while (hdr_sent < hdr_len) {
        int n = send(client, header + hdr_sent, hdr_len - hdr_sent, 0);
        if (n <= 0) return;
        hdr_sent += n;
    }
    // Send body in chunks (may be large)
    int sent = 0;
    while (sent < body_len) {
        int n = send(client, body + sent, body_len - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}

// ─── Client handler ───

static bool query_u32(const char *path, const char *key, uint32_t *value) {
    const char *query = strchr(path, '?');
    if (!query) return false;
    query++;

    size_t key_len = strlen(key);
    while (*query) {
        if (strncmp(query, key, key_len) == 0 && query[key_len] == '=') {
            char *end = NULL;
            unsigned long v = strtoul(query + key_len + 1, &end, 10);
            if (end == query + key_len + 1) return false;
            *value = (uint32_t)v;
            return true;
        }

        query = strchr(query, '&');
        if (!query) break;
        query++;
    }
    return false;
}

static void send_api_json(int client, const char *body) {
    send_http_response(client, "200 OK", "application/json", body, strlen(body));
}

static void send_api_error(int client, const char *status, const char *message) {
    char body[96];
    int len = snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", message);
    send_http_response(client, status, "application/json", body, len);
}

static bool handle_api_request(int client, const char *method, const char *path, const char *body) {
    if (strncmp(path, "/api/", 5) != 0) return false;

    uint16_t batt_mv = 0;
    bool charging = false;
    uint32_t baud = s_get_baud_cb ? s_get_baud_cb() : 0;

    if (strncmp(path, "/api/status", 11) == 0) {
        esp_err_t batt_ret = sticks3_power_get_battery(&batt_mv);
        sticks3_power_is_charging(&charging);

        char body[160];
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"baud\":%u,\"batt_mv\":%u,\"charging\":%s,\"batt_ok\":%s}",
                 (unsigned)baud, (unsigned)batt_mv,
                 charging ? "true" : "false",
                 batt_ret == ESP_OK ? "true" : "false");
        send_api_json(client, body);
        return true;
    }

    if (strncmp(path, "/api/battery", 12) == 0) {
        esp_err_t batt_ret = sticks3_power_get_battery(&batt_mv);
        sticks3_power_is_charging(&charging);
        if (batt_ret != ESP_OK) {
            send_api_error(client, "500 Internal Server Error", "battery_read_failed");
            return true;
        }

        char body[128];
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"batt_mv\":%u,\"charging\":%s}",
                 (unsigned)batt_mv, charging ? "true" : "false");
        send_api_json(client, body);
        return true;
    }

    if (strncmp(path, "/api/baud", 9) == 0) {
        uint32_t new_baud = 0;
        if (query_u32(path, "value", &new_baud) || query_u32(path, "baud", &new_baud)) {
            if (!s_set_baud_cb || !s_set_baud_cb(new_baud)) {
                send_api_error(client, "400 Bad Request", "unsupported_baud");
                return true;
            }
            baud = new_baud;
        }

        char body[96];
        snprintf(body, sizeof(body), "{\"ok\":true,\"baud\":%u}", (unsigned)baud);
        send_api_json(client, body);
        return true;
    }

    if (strncmp(path, "/api/macros", 11) == 0) {
        if (strcmp(method, "GET") == 0) {
            char *macros = malloc(MACROS_JSON_MAX);
            char *resp = malloc(MACROS_JSON_MAX + 32);
            if (!macros || !resp) {
                free(macros);
                free(resp);
                send_api_error(client, "500 Internal Server Error", "out_of_memory");
                return true;
            }

            sticks3_nvs_load_macros(macros, MACROS_JSON_MAX);
            snprintf(resp, MACROS_JSON_MAX + 32, "{\"ok\":true,\"macros\":%s}", macros);
            send_api_json(client, resp);
            free(macros);
            free(resp);
            return true;
        }

        if (strcmp(method, "POST") == 0) {
            if (!body || strlen(body) >= MACROS_JSON_MAX) {
                send_api_error(client, "400 Bad Request", "macros_too_large");
                return true;
            }
            if (body[0] != '[') {
                send_api_error(client, "400 Bad Request", "expected_json_array");
                return true;
            }
            if (sticks3_nvs_save_macros(body) != ESP_OK) {
                send_api_error(client, "500 Internal Server Error", "save_failed");
                return true;
            }
            send_api_json(client, "{\"ok\":true}");
            return true;
        }

        send_api_error(client, "405 Method Not Allowed", "method_not_allowed");
        return true;
    }

    send_api_error(client, "404 Not Found", "not_found");
    return true;
}

static void handle_client(int client) {
    const int buf_size = MACROS_JSON_MAX + 768;
    char *buf = malloc(buf_size);
    if (!buf) {
        send_http_response(client, "500 Internal Server Error", "text/plain", "Out of memory", 13);
        close(client);
        return;
    }

    int len = recv(client, buf, buf_size - 1, 0);
    if (len <= 0) {
        free(buf);
        close(client);
        return;
    }
    buf[len] = '\0';

    char method[8] = {0};
    char path[128] = {0};
    if (sscanf(buf, "%7s %127s", method, path) != 2) {
        send_http_response(client, "400 Bad Request", "text/plain", "Bad Request", 11);
        free(buf);
        close(client);
        return;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
        send_http_response(client, "405 Method Not Allowed", "text/plain", "Method Not Allowed", 18);
        free(buf);
        close(client);
        return;
    }

    char *body = strstr(buf, "\r\n\r\n");
    int body_len = 0;
    int content_len = 0;
    if (body) {
        body += 4;
        body_len = len - (int)(body - buf);
    }
    char *cl = strstr(buf, "Content-Length:");
    if (cl) {
        content_len = atoi(cl + 15);
    }
    if (content_len > MACROS_JSON_MAX) {
        send_api_error(client, "413 Payload Too Large", "payload_too_large");
        free(buf);
        close(client);
        return;
    }
    while (body && content_len > body_len && len < buf_size - 1) {
        int n = recv(client, buf + len, buf_size - 1 - len, 0);
        if (n <= 0) break;
        len += n;
        buf[len] = '\0';
        body = strstr(buf, "\r\n\r\n");
        if (body) {
            body += 4;
            body_len = len - (int)(body - buf);
        }
    }
    if (body && content_len > body_len) {
        send_api_error(client, "400 Bad Request", "incomplete_body");
        free(buf);
        close(client);
        return;
    }
    if (body && content_len >= 0 && body_len >= content_len) {
        body[content_len] = '\0';
    }

    if (handle_api_request(client, method, path, body)) {
        free(buf);
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
            free(buf);
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
    free(buf);
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
    xTaskCreate(http_server_task, "http_srv", 12288, NULL, 4, &s_http_task);
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

void sticks3_http_server_set_baud_cbs(sticks3_http_get_baud_cb_t get_cb,
                                      sticks3_http_set_baud_cb_t set_cb) {
    s_get_baud_cb = get_cb;
    s_set_baud_cb = set_cb;
}
