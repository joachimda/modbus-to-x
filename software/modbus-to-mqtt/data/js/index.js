import {API, safeGet} from "app";

window.initIndex = async function initIndex() {
    $("#btn-reboot").addEventListener("click", reboot);
    await render();
    await tickHealth();
    setInterval(await render, 30000, );
    setInterval(tickHealth, 30000);
}
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

// --- Render functions ---
async function render() {
    const [sys/*, wifi, mqtt, mdb, stg*/] = await Promise.all([
        safeGet(API.SYSTEM_STATS),
        // safeGet(API.NETWORK_STATS),
        // safeGet(API.MQTT_STATS),
        // safeGet(API.MODBUS_STATS),
        // safeGet(API.STORAGE_STATS),
    ]);

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
    const wifiDot = sys.__error ? dot("bad", "Unknown") :
        sys.connected ? dot("ok", "Connected") :
            sys.apMode ? dot("warn", "AP mode") : dot("bad", "Disconnected");
    $("#wifi-kvs").innerHTML = sys.__error ? kv("Error", sys.__error) : [
        kv("Status", wifiDot),
        kv("SSID", sys.ssid || "—"),
        kv("IP", sys.ip || "—"),
        kv("RSSI", sys.rssi == null ? "—" : `${sys.rssi} dBm`),
        kv("MAC", sys.mac || "—"),
    ].join("");

    // MQTT card
    const mqttDot = sys.__error ? dot("bad", "Unknown") :
        sys.connected ? dot("ok", "Connected") :
            dot("bad", "Disconnected");
    $("#mqtt-kvs").innerHTML = sys.__error ? kv("Error", sys.__error) : [
        kv("Status", mqttDot),
        kv("Broker", sys.broker || "—"),
        kv("Client ID", sys.clientId || "—"),
        kv("Last publish", sys.lastPublishIso || "—"),
        kv("Errors", sys.errorCount ?? 0),
    ].join("");

    // Modbus card
    $("#modbus-kvs").innerHTML = sys.__error ? kv("Error", sys.__error) : [
        kv("Buses", sys.buses ?? "—"),
        kv("Devices", sys.devices ?? "—"),
        kv("Datapoints", sys.datapoints ?? "—"),
        kv("Poll interval", sys.pollIntervalMs ? `${sys.pollIntervalMs} ms` : "—"),
        kv("Last poll", sys.lastPollIso || "—"),
        kv("Errors", sys.errorCount ?? 0),
    ].join("");

    // Storage card
    $("#storage-kvs").innerHTML = sys.__error ? kv("Error", sys.__error) : [
        kv("Flash size", prettyBytes(sys.flashSize)),
        kv("SPIFFS used", `${prettyBytes(sys.spiffsUsed)} / ${prettyBytes(sys.spiffsTotal)}`),
        kv("SPIFFS usage", sys.spiffsTotal ? fmtPct(100 * (sys.spiffsUsed / sys.spiffsTotal)) : "—"),
    ].join("");
}

async function tickHealth() {
    const r = await safeGet("/api/health");
    if (r.__error) {
        $("#health-line").textContent = `Health: ${r.__error}`;
        return;
    }
    const parts = [];
    if (r.ok) parts.push("OK");
    if (r.components) {
        Object.keys(r.components).forEach(k => {
            const s = r.components[k];
            const cls = s === "ok" ? "ok" : (s === "warn" ? "warn" : (s === "idle" ? "idle" : "bad"));
            parts.push(`${dot(cls, k)}`);
        });
    }
    $('#health-line').innerHTML = parts.join(" • ");
}

async function reboot() {
    if (!confirm("Reboot the device now?")) return;
    try {
        const r = await fetch("/api/system/reboot", { method: "POST" });
        if (r.ok) {
            alert("Rebooting… The page will try to reconnect automatically.");
            // Optional: try to reload after a short pause
            setTimeout(() => location.reload(), 5000);
        } else {
            alert("Reboot request failed.");
        }
    } catch (e) {
        alert("Reboot request failed: " + e.message);
    }
}

