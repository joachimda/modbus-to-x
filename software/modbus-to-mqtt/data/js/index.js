import {API, safeGet, reboot} from "app";

const $ = (sel) => document.querySelector(sel);
const kv = (k, v) => `<div class="key">${k}</div><div>${v ?? "—"}</div>`;
const dot = (cls, label) => `<span class="status-dot ${cls}"></span>${label}`;

function prettyBytes(bytes) {
    if (bytes == null) return "—";
    const units = ["B","KB","MB","GB"];
    let i = 0, b = Number(bytes);
    while (b >= 1024 && i < units.length - 1) { b /= 1024; i++; }
    return `${b.toFixed(b >= 10 || i === 0 ? 0 : 1)} ${units[i]}`;
}
const fmtPct = (x) => (x == null ? "—" : `${x.toFixed(0)}%`);
const fmtMs = (ms) => {
    if (ms == null) return "—";
    const s = Math.floor(ms/1000), d = Math.floor(s/86400), h = Math.floor((s%86400)/3600), m = Math.floor((s%3600)/60);
    const r = [];
    if (d) r.push(`${d}d`);
    if (h || d) r.push(`${h}h`);
    r.push(`${m}m`);
    return r.join(" ");
};

let eventSource = null;
let logsBuffer = "";
let statsState = {};
const MAX_LOG_CHARS = 16000;

window.initIndex = async function initIndex() {
    $("#btn-reboot").addEventListener("click", reboot);
    setupLogs();
    startEventStream();

    // Light fallback so the page renders even before the stream delivers the first event
    const sys = await safeGet(API.SYSTEM_STATS);
    if (!sys.__error) updateStats(sys);
    else renderStats(sys);
};

function startEventStream() {
    if (eventSource) eventSource.close();
    eventSource = new EventSource(API.EVENTS);

    const statEvents = [
        "stats-system",
        "stats-network",
        "stats-mqtt",
        "stats-modbus",
        "stats-storage",
        "stats-health",
        "stats", // fallback if server sends aggregated payload
    ];
    statEvents.forEach((name) => {
        eventSource.addEventListener(name, (ev) => handleStatsEvent(ev));
    });
    eventSource.addEventListener("logs", (ev) => handleLogEvent(ev, true));
    eventSource.addEventListener("log", (ev) => handleLogEvent(ev, false));
    eventSource.addEventListener("error", () => {
        const subtitle = $("#device-subtitle");
        if (subtitle && !subtitle.dataset.streamWarned) {
            subtitle.dataset.streamWarned = "true";
            subtitle.textContent = `${subtitle.textContent || "Device"} • live stream reconnecting…`;
        }
    });
}

function handleLogEvent(ev, replace) {
    try {
        const payload = JSON.parse(ev.data || "{}");
        const text = typeof payload.text === "string" ? payload.text : "";
        const truncated = !!payload.truncated;
        applyLogs(text, { replace, truncated });
    } catch (err) {
        console.error("Failed to parse log event", err);
    }
}

function handleStatsEvent(ev) {
    try {
        const payload = JSON.parse(ev.data || "{}");
        updateStats(payload);
    } catch (err) {
        console.error("Failed to parse stats event", err);
    }
}

function updateStats(partial) {
    if (!partial || typeof partial !== "object") return;
    statsState = { ...statsState, ...partial };
    renderStats(statsState);
}

function applyLogs(text, { replace = false, truncated = false } = {}) {
    if (replace) {
        logsBuffer = text || "";
    } else {
        logsBuffer += text || "";
    }
    if (logsBuffer.length > MAX_LOG_CHARS) {
        logsBuffer = logsBuffer.slice(logsBuffer.length - MAX_LOG_CHARS);
        truncated = true;
    }
    renderLogs({ forceScroll: replace, truncated });
}

function renderLogs({ forceScroll = false, truncated = false } = {}) {
    const el = $("#logs-console");
    const panel = $("#logs-panel");
    if (!el) return;
    if (truncated) el.dataset.truncated = "true";
    if (panel && !panel.open && !forceScroll) return;

    const atBottom = Math.abs(el.scrollHeight - el.scrollTop - el.clientHeight) < 30;
    el.textContent = logsBuffer || "(no logs)";
    if (forceScroll || atBottom) {
        el.scrollTop = el.scrollHeight;
    }
}

function setupLogs() {
    const panel = $("#logs-panel");
    const refreshBtn = $("#btn-refresh-logs");
    if (panel) {
        panel.addEventListener("toggle", () => {
            if (panel.open) {
                const truncated = $("#logs-console")?.dataset?.truncated === "true";
                renderLogs({ forceScroll: true, truncated });
            }
        });
    }
    if (refreshBtn) refreshBtn.addEventListener("click", (e) => {
        e.preventDefault();
        fetchLogsSnapshot(true);
    });
}

async function fetchLogsSnapshot(forceScroll = false) {
    const el = $("#logs-console");
    if (!el) return;
    try {
        const r = await fetch(API.GET_LOGS, { cache: "no-store" });
        if (!r.ok) {
            throw new Error(`${r.status} ${r.statusText}`);
        }
        const text = await r.text();
        applyLogs(text, { replace: true });
        renderLogs({ forceScroll });
    } catch (e) {
        el.textContent = `Error loading logs: ${e.message}`;
    }
}

function renderStats(sys = {}) {
    // Header subtitle
    if (!sys.__error) {
        $("#device-subtitle").textContent =
            `${sys.deviceName || "Device"} • ${sys.chipModel || "ESP32"} • ${sys.ip || "—"}`;
        $("#fw-version").textContent = sys.fwVersion || "—";
        $("#build-date").textContent = sys.buildDate || "—";
    } else {
        $("#device-subtitle").textContent = "System info unavailable";
    }

    // System card
    $("#system-kvs").innerHTML = sys.__error ? kv("Error", sys.__error) : [
        kv("Uptime", fmtMs(sys.uptimeMs)),
        kv("CPU", `${sys.cpuFreqMHz ?? "—"} MHz`),
        kv("SDK", sys.sdkVersion || "—"),
        kv("Heap (free/min)", `${prettyBytes(sys.heapFree)} / ${prettyBytes(sys.heapMin)}`),
        kv("Reset reason", sys.resetReason || "—"),
    ].join("");

    // Wi-Fi card
    const wifiConnected = (sys.wifiConnected !== undefined) ? sys.wifiConnected : sys.connected;
    const wifiApMode = (sys.wifiApMode !== undefined) ? sys.wifiApMode : sys.apMode;
    const wifiDot = sys.__error ? dot("bad", "Unknown") :
        wifiConnected ? dot("ok", "Connected") :
            wifiApMode ? dot("warn", "AP mode") : dot("bad", "Disconnected");
    $("#wifi-kvs").innerHTML = sys.__error ? kv("Error", sys.__error) : [
        kv("Status", wifiDot),
        kv("SSID", sys.ssid || "—"),
        kv("IP", sys.ip || "—"),
        kv("RSSI", sys.rssi == null ? "—" : `${sys.rssi} dBm`),
        kv("MAC", sys.mac || "—"),
    ].join("");

    // MQTT card
    const mqttConnected = (sys.mqttConnected !== undefined) ? sys.mqttConnected : false;
    const mqttDot = sys.__error ? dot("bad", "Unknown") :
        mqttConnected ? dot("ok", "Connected") :
            dot("bad", "Disconnected");
    $("#mqtt-kvs").innerHTML = sys.__error ? kv("Error", sys.__error) : [
        kv("Status", mqttDot),
        kv("Broker", sys.broker || "—"),
        kv("Client ID", sys.clientId || "—"),
        kv("Errors", sys.mqttErrorCount ?? "—"),
    ].join("");

    // Modbus card
    const mbusEnabled = (sys.mbusEnabled !== undefined) ? sys.mbusEnabled : false;
    const mbusDot = sys.__error ? dot("bad", "Unknown") :
        mbusEnabled ? dot("ok", "Enabled") :
            dot("bad", "Disabled");
    $("#modbus-kvs").innerHTML = sys.__error ? kv("Error", sys.__error) : [
        kv("Status", mbusDot),
        kv("Buses", sys.buses ?? "—"),
        kv("Devices", sys.devices ?? "—"),
        kv("Datapoints", sys.datapoints ?? "—"),
        kv("Errors", sys.modbusErrorCount ?? "—"),
    ].join("");

    // Storage card
    $("#storage-kvs").innerHTML = sys.__error ? kv("Error", sys.__error) : [
        kv("Device Flash size", prettyBytes(sys.flashSize)),
        kv("Free Space", `${prettyBytes(sys.configUsed)}/${prettyBytes(sys.configTotal)} (${fmtPct(100 * (sys.configUsed / sys.configTotal))})`),
    ].join("");
}
