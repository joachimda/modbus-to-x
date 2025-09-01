// === Network Config API ===
export const API = {
    SSIDS: '/api/ssids',
    CONNECT: '/api/wifi/connect',
    STATUS: '/api/wifi/status',
    CANCEL: '/api/wifi/cancel',
    NETWORK_STATS: '/api/stats/network',
    SYSTEM_STATS: '/api/stats/system',
    STORAGE_STATS: '/api/stats/storage',
    MODBUS_STATS: '/api/stats/modbus',
    MQTT_STATS: '/api/stats/mqtt'
};

document.addEventListener('DOMContentLoaded', async () => {
    const page = document.body?.dataset?.page;
    if (page === 'index' && typeof window.initIndex === 'function') await window.initIndex();
    if (page === 'configure_v2' && typeof window.initConfigureV2 === 'function') await window.initConfigureV2();
    if (page === 'configure_network' && typeof window.initConfigureNetwork === 'function') await window.initConfigureNetwork();
});

// Signal bars & badges
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
   Helpers
   =========================== */
export async function safeGet(url) {
    try {
        const r = await fetch(url, {cache: "no-cache"});
        if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
        return await r.json();
    } catch (e) {
        return { __error: e.message };
    }
}
export const kebab = (s) => (s || "").toString().trim().toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g,"");

export async function safeJson(url, init) {
    const r = await fetch(url, { cache: "no-cache", ...init });
    if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
    if (r.status === 204) return {};
    return await r.json();
}

export function setText(el, t){ if (el) el.textContent = t; }
export function pretty(obj){ return JSON.stringify(obj, null, 2); }
export function $id(id){ return document.getElementById(id); }