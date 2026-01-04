export const API = {
    SSIDS: '/api/ssids',
    CONNECT: '/api/wifi/connect',
    STATUS: '/api/wifi/status',
    CANCEL: '/api/wifi/cancel',
    SYSTEM_STATS: '/api/stats/system',
    EVENTS: '/api/events',
    GET_LOGS: '/api/logs',
    PUT_MODBUS_CONFIG: '/api/config/modbus',
    PUT_MQTT_CONFIG: '/api/config/mqtt',
    PUT_MQTT_SECRET: '/api/config/mqtt/secret',
    POST_MQTT_TEST: '/api/mqtt/test',
    POST_MODBUS_EXECUTE: '/api/modbus/execute',
    POST_SYSTEM_RESET: '/api/system/reboot',
    OTA_HTTP_CHECK: '/api/system/ota/http/check',
    OTA_HTTP_APPLY: '/api/system/ota/http/apply',
};

export const STATIC_FILES = {
    MODBUS_CONFIG_JSON: '/conf/config.json',
    MQTT_CONFIG_JSON: '/conf/mqtt.json',
    CONFIG_SCHEMA_JSON: '/conf/schema.json',
}

document.addEventListener('DOMContentLoaded', async () => {
    const page = document.body?.dataset?.page;
    if (page === 'index' && typeof window.initIndex === 'function') await window.initIndex();
    if (page === 'configure_modbus' && typeof window.initConfigureModbus === 'function') await window.initConfigureModbus();
    if (page === 'mbx_captive_portal' && typeof window.initCaptivePortal === 'function') await window.initCaptivePortal();
    if (page === 'configure_mqtt' && typeof window.initConfigureMqtt === 'function') await window.initConfigureMqtt();
});
export async function reboot() {
    if (!confirm("Reboot the device now?")) return;
    try {
        const r = await fetch(API.POST_SYSTEM_RESET, { method: "POST" });
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
export function rssiToBars(rssi) {
    const level = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
    return '📶'.repeat(level) + '·'.repeat(4 - level);
}

export function rssiBadge(rssi) {
    const cls = rssi >= -60 ? 'ok' : rssi >= -72 ? 'warn' : 'bad';
    const label = rssi >= -60 ? 'Strong' : rssi >= -72 ? 'Fair' : 'Weak';
    return `<span class="badge ${cls}">${label}</span>`;
}


/* ===========================
   Request Helpers
   =========================== */
export async function safeGet(url) {
    try {
        const r = await fetch(url, {cache: "no-cache"});
        if (!r.ok) {
            throw new Error(`${r.status} ${r.statusText}`);
        }
        return await r.json();
    }
    catch (e) {
        return { __error: e.message };
    }
}
export async function safeJson(url, init) {
    const r = await fetch(url, { cache: "no-cache", ...init });
    if (!r.ok) {
        throw new Error(`${r.status} ${r.statusText}`);
    }
    if (r.status === 204) {
        return {};
    }
    return await r.json();
}
