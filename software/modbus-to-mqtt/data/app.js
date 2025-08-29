/* app.js — Modbus To X UI
   Structure:
   - Router
   - Helpers (shared)
   - Index page (els + init + logic)
   - Configure page (els + init + logic)
   - Configure Network page
*/

/* ===========================
   Router
   =========================== */
document.addEventListener('DOMContentLoaded', () => {
    const page = document.body?.dataset?.page;
    if (page === 'index' && typeof initIndex === 'function') initIndex();
    if (page === 'configure' && typeof initConfigure === 'function') initConfigure();
});

/* ===========================
   Helpers (shared)
   =========================== */
function $id(id){ return document.getElementById(id); }
function setText(el, t){ if (el) el.textContent = t; }
function pretty(obj){ return JSON.stringify(obj, null, 2); }

function setStatus(el, msg, cls='') {
    if (!el) return;
    el.textContent = msg;
    el.className = `status ${cls}`;
}

function fillSelect(select, values) {
    if (!select) return;
    select.innerHTML = '';
    for (const v of values) {
        const opt = document.createElement('option');
        opt.value = v; opt.textContent = v;
        select.appendChild(opt);
    }
}

function slug(s) {
    return String(s || '')
        .trim()
        .toLowerCase()
        .replace(/[^a-z0-9]+/g, '_')
        .replace(/^_+|_+$/g, '')
        .replace(/_+/g, '_');
}

function makeDpId(deviceName, dpName) {
    const d = slug(deviceName);
    const n = slug(dpName);
    return d && n ? `${d}.${n}` : '';
}

function previewDpId(deviceName, dpName) {
    const id = makeDpId(deviceName, dpName);
    return id ? `id: ${id}` : `id: (enter names to generate)`;
}

/* ===========================
   Index page
   =========================== */
let elsIndex = null; // DOM refs only

function initIndex() {
    elsIndex = {
        chipId: $id('chipId'),
        ssid:   $id('ssid')
    };

    // (placeholders for now)
    setText(elsIndex.chipId, 'unknown');
    setText(elsIndex.ssid, 'unknown');
}

/* ===========================
   Configure page
   =========================== */
const BAUDS = [1200,2400,4800,9600,19200,38400,57600,115200,230400,460800,921600];
const SERIAL_FORMATS = [
    "5N1","6N1","7N1","8N1","5N2","6N2","7N2","8N2",
    "5E1","6E1","7E1","8E1","5E2","6E2","7E2","8E2",
    "5O1","6O1","7O1","8O1","5O2","6O2","7O2","8O2"
];
const DP_REGISTER_TYPES = ["H","I"];
const DP_DATA_TYPES     = ["TEXT","F32","S16","DATE"];
const DP_ACCESS         = ["r","rw"];

let elsCfg = null;   // DOM refs only
let devEls = null;   // DOM refs for a device/Datapoint's section only

function initConfigure() {
    elsCfg = {
        // toolbar + status
        btnLoad:  $id('btnLoad'),
        btnSave:  $id('btnSave'),
        status:   $id('status'),

        bus1_baud:   $id('bus1_baud'),
        bus1_format: $id('bus1_format'),
        bus1_term:   $id('bus1_term'),
        bus1_bias:   $id('bus1_bias'),

        // advanced JSON
        advJson:  $id('advJson'),
        jsonBox:  $id('jsonBox'),
        btnApply: $id('btnApply'),
        btnFormat:$id('btnFormat')
    };

    devEls = {
        // device basics
        name:  $id('dev_name'),
        slave: $id('dev_slave'),
        order: $id('dev_order'),
        bus:   $id('dev_bus'),

        // Datapoint table + controls
        dpBody:        $id('dpBody'),
        btnAddDpRow:   $id('btnAddDpRow'),
        btnClearDp:    $id('btnClearDpRows'),
        btnAddDevice:  $id('btnAddDevice'),
    };

    /* 2) One-time control population */
    fillSelect(elsCfg.bus1_baud, BAUDS);
    fillSelect(elsCfg.bus1_format, SERIAL_FORMATS);

    /* 3) Event listeners */
    if (elsCfg.btnLoad)  elsCfg.btnLoad.addEventListener('click', loadConfig);
    if (elsCfg.btnFormat)elsCfg.btnFormat.addEventListener('click', formatJson);
    if (elsCfg.btnApply) elsCfg.btnApply.addEventListener('click', applyEditorChanges);
    if (elsCfg.btnSave)  elsCfg.btnSave.addEventListener('click', saveToDevice);

    // JSON sync when bus controls change
    for (const id of ['bus1_baud','bus1_format','bus1_term','bus1_bias']) {
        const el = $id(id);
        if (el) el.addEventListener('change', updateEditorFromForm);
    }

    // device & datapoint handlers
    if (devEls.btnAddDpRow) devEls.btnAddDpRow.addEventListener('click', () => addDpRow());
    if (devEls.btnClearDp)  devEls.btnClearDp.addEventListener('click', clearDpRows);
    if (devEls.btnAddDevice)devEls.btnAddDevice.addEventListener('click', addDeviceToJson);
    if (devEls.name)        devEls.name.addEventListener('input', updateAllDpIdPreviews);

    // start with one datapoint row
    clearDpRows();
    addDpRow();
    updateAllDpIdPreviews();

    /* 4) Initial data load */
    loadConfig();
}

/* ---------- Configure: load/format/save ---------- */
async function loadConfig() {
    setStatus(elsCfg.status, 'Loading configuration...', '');
    let resp = await fetch('/download/config', { cache: 'no-store' });
    if (!resp.ok) {
        resp = await fetch('/download/config_example', { cache: 'no-store' });
        if (!resp.ok) {
            setStatus(elsCfg.status, 'Failed to load configuration (and example).', 'err');
            return;
        } else {
            setStatus(elsCfg.status, 'Loaded example configuration (no user config yet).', 'warn');
        }
    } else {
        setStatus(elsCfg.status, 'Loaded current configuration from device.', 'ok');
    }

    try {
        const cfg = await resp.json();
        elsCfg.jsonBox.value = pretty(cfg);
        applyBusesToForm(cfg);
    } catch (e) {
        setStatus(elsCfg.status, 'Error parsing JSON: ' + e.message, 'err');
    }
}

function formatJson() {
    try {
        elsCfg.jsonBox.value = pretty(JSON.parse(elsCfg.jsonBox.value));
        setStatus(elsCfg.status, 'JSON formatted.', 'ok');
    } catch (e) {
        setStatus(elsCfg.status, 'Invalid JSON: ' + e.message, 'err');
    }
}

function applyEditorChanges() {
    try {
        const cfg = JSON.parse(elsCfg.jsonBox.value);
        applyBusesToForm(cfg);
        setStatus(elsCfg.status, 'Editor applied to form.', 'ok');
    } catch (e) {
        setStatus(elsCfg.status, 'Invalid JSON: ' + e.message, 'err');
    }
}

function updateEditorFromForm() {
    try {
        const cfg = JSON.parse(elsCfg.jsonBox.value || '{}');
        applyFormToBuses(cfg);
        elsCfg.jsonBox.value = pretty(cfg);
        setStatus(elsCfg.status, 'Form changes applied to JSON.', 'ok');
    } catch (e) {
        setStatus(elsCfg.status, 'Invalid JSON before applying form: ' + e.message, 'err');
    }
}

async function saveToDevice() {
    let cfg;
    try {
        cfg = JSON.parse(elsCfg.jsonBox.value);
    } catch (e) {
        setStatus(elsCfg.status, 'Cannot save: JSON is invalid. ' + e.message, 'err');
        return;
    }
    if (!cfg.version) cfg.version = 1;

    const blob = new Blob([pretty(cfg)], { type: 'application/json' });
    const form = new FormData();
    form.append('file', blob, 'config.json');

    setStatus(elsCfg.status, 'Uploading config...', '');
    try {
        const resp = await fetch('/upload', { method: 'POST', body: form });
        if (resp.ok) {
            setStatus(elsCfg.status, 'Config uploaded successfully.', 'ok');
        } else {
            const t = await resp.text();
            setStatus(elsCfg.status, 'Upload failed: ' + t, 'err');
        }
    } catch (e) {
        setStatus(elsCfg.status, 'Upload error: ' + e.message, 'err');
    }
}

/* ---------- Configure: buses <-> JSON ---------- */
function applyBusesToForm(cfg) {
    if (cfg.bus1) {
        if (elsCfg.bus1_baud)   elsCfg.bus1_baud.value   = String(cfg.bus1.baud ?? '');
        if (elsCfg.bus1_format) elsCfg.bus1_format.value = cfg.bus1.serialFormat ?? '';
        if (elsCfg.bus1_term)   elsCfg.bus1_term.value   = cfg.bus1.termination ?? '120R';
        if (elsCfg.bus1_bias)   elsCfg.bus1_bias.checked = !!cfg.bus1.enableBias;
    }
}

function applyFormToBuses(cfg) {
    cfg.bus1 = cfg.bus1 || {};
    if (elsCfg.bus1_baud)   cfg.bus1.baud         = Number(elsCfg.bus1_baud.value);
    if (elsCfg.bus1_format) cfg.bus1.serialFormat = elsCfg.bus1_format.value;
    if (elsCfg.bus1_term)   cfg.bus1.termination  = elsCfg.bus1_term.value;
    if (elsCfg.bus1_bias)   cfg.bus1.enableBias   = !!elsCfg.bus1_bias.checked;
}

/* ---------- Configure: device + datapoint's ---------- */
function mkInput(placeholder, type="text") {
    const i = document.createElement('input');
    i.type = type;
    i.placeholder = placeholder;
    return i;
}

function makeSelect(options, value) {
    const sel = document.createElement('select');
    for (const o of options) {
        const opt = document.createElement('option');
        opt.value = o; opt.textContent = o;
        sel.appendChild(opt);
    }
    if (value != null) sel.value = value;
    return sel;
}

function addDpRow(prefill = {}) {
    const tr = document.createElement('tr');

    // Name cell with generated-id preview
    const td_name = document.createElement('td');
    td_name.className = 'name-cell';
    const name = mkInput('Voltage');
    name.value = prefill.name ?? '';
    const nameHint = document.createElement('div');
    nameHint.className = 'hint';
    nameHint.textContent = ''; // set below
    td_name.appendChild(name);
    td_name.appendChild(nameHint);

    const td_address = document.createElement('td');
    const inp_address = mkInput('1234');
    inp_address.value = prefill.address ?? '';
    td_address.appendChild(inp_address);

    const td_numRegisters = document.createElement('td');
    const inp_numRegisters   = mkInput('1');
    inp_numRegisters.value   = prefill.numOfRegisters ?? '1';
    td_numRegisters.appendChild(inp_numRegisters);

    const td_scale= document.createElement('td');
    const inp_scale   = mkInput('1.0');
    inp_scale.value   = prefill.scale ?? '';
    td_scale.appendChild(inp_scale);

    const td_registerType= document.createElement('td');
    const inp_registerType= makeSelect(DP_REGISTER_TYPES, prefill.registerType);
    td_registerType.appendChild(inp_registerType);

    const td_dataType= document.createElement('td');
    const inp_dataType= makeSelect(DP_DATA_TYPES, prefill.dataType);
    td_dataType.appendChild(inp_dataType);

    const td_unit= document.createElement('td');
    const inp_unit = mkInput('V');
    inp_unit.value = prefill.unit ?? '';
    td_unit.appendChild(inp_unit);

    const td_access= document.createElement('td');
    const inp_access= makeSelect(DP_ACCESS, prefill.access);
    td_access.appendChild(inp_access);

    const td_poll= document.createElement('td');
    const poll = mkInput('1000');
    poll.value = prefill.pollMs ?? '';
    td_poll.appendChild(poll);

    const td_deadband= document.createElement('td');
    const inp_deadband= mkInput('0');
    inp_deadband.value = prefill.deadBand ?? ''; td_deadband.appendChild(inp_deadband);

    const td_precision= document.createElement('td');
    const inp_precision= mkInput('1');
    inp_precision.value = prefill.precision ?? '';
    td_precision.appendChild(inp_precision);

    const td_del= document.createElement('td');
    const delBtn= document.createElement('button');
    delBtn.textContent = '✕';
    delBtn.className = 'btn danger';
    delBtn.style.padding = '6px 10px';
    delBtn.addEventListener('click', () => tr.remove());
    td_del.appendChild(delBtn);

    tr._cells = {
        name,
        nameHint,
        address: inp_address,
        numOfRegisters: inp_numRegisters,
        scale: inp_scale,
        registerType: inp_registerType,
        dataType: inp_dataType,
        unit: inp_unit,
        access: inp_access,
        poll,
        deadBand: inp_deadband,
        precision: inp_precision };
    tr.append(td_name, td_address, td_numRegisters, td_scale, td_registerType, td_dataType, td_unit, td_access, td_poll, td_deadband, td_precision, td_del);
    devEls.dpBody.appendChild(tr);

    // Live preview as names change
    name.addEventListener('input', () => {
        tr._cells.nameHint.textContent = previewDpId(devEls.name.value, name.value);
    });
    tr._cells.nameHint.textContent = previewDpId(devEls.name.value, name.value);
}

function clearDpRows() {
    if (devEls.dpBody) devEls.dpBody.innerHTML = '';
}

function updateAllDpIdPreviews() {
    if (!devEls?.dpBody) return;
    const deviceName = devEls.name?.value || '';
    const rows = [...devEls.dpBody.querySelectorAll('tr')];
    for (const r of rows) {
        const dpName = r._cells?.name?.value || '';
        r._cells.nameHint.textContent = previewDpId(deviceName, dpName);
    }
}

function readDatapoints(deviceName) {
    const body = devEls.dpBody;
    if (!body) return { dps: [] };
    const rows = [...body.querySelectorAll('tr')];
    const datapoints = [];
    for (const r of rows) {
        const c = r._cells;
        const dpName = c.name.value.trim();
        const obj = {
            id:      makeDpId(deviceName, dpName),
            name:    dpName,
            address: Number(c.address.value),
            numOfRegisters: Number(c.numOfRegisters.value || 1),
            scale:   c.scale.value === '' ? undefined : Number(c.scale.value),
            registerType: c.registerType.value,
            dataType: c.dataType.value,
            unit:    c.unit.value.trim(),
            access:  c.access.value,
            pollMs:  c.poll.value === '' ? undefined : Number(c.poll.value),
            deadBand: c.deadBand.value === '' ? undefined : Number(c.deadBand.value),
            precision: c.precision.value === '' ? undefined : Number(c.precision.value),
        };

        // validation
        if (!obj.name) return { error: 'Datapoint name is required' };
        if (!obj.id)
        {
            return { error: 'Generated datapoint ID is empty; check device & datapoint names' };
        }
        if (Number.isNaN(obj.address) || obj.address < 0 || obj.address > 65535)
            return { error: `Address must be 0–65535 (got ${c.address.value})` };
        if (Number.isNaN(obj.numOfRegisters) || obj.numOfRegisters < 1 || obj.numOfRegisters > 125)
            return { error: `#Regs must be 1–125 (got ${c.numOfRegisters.value})` };
        if (!DP_REGISTER_TYPES.includes(obj.registerType))
            return { error: 'Invalid register type' };
        if (!DP_DATA_TYPES.includes(obj.dataType))
            return { error: 'Invalid data type' };
        if (!DP_ACCESS.includes(obj.access))
            return { error: 'Invalid access' };

        // prune undefined for tidy JSON
        for (const k of Object.keys(obj)) if (obj[k] === undefined || obj[k] === '') delete obj[k];
        datapoints.push(obj);
    }
    return { dps: datapoints };
}

function buildDeviceFromForm() {
    const name = devEls.name?.value.trim();
    const slave = Number(devEls.slave?.value);
    const byteOrder = devEls.order?.value;
    const busId = Number(devEls.bus?.value || 1);

    if (!name) return { error: 'Device name is required' };
    if (Number.isNaN(slave) || slave < 1 || slave > 247) return { error: 'Slave ID must be 1–247' };
    if (!['ABCD','DCBA'].includes(byteOrder)) return { error: 'Invalid byte order' };
    if (busId !== 1) return { error: 'Only bus1 supported right now' };

    const { dps, error } = readDatapoints(name);
    if (error) return { error };

    // prevent duplicate IDs within the new device
    const seen = new Set();
    for (const dp of dps) {
        if (seen.has(dp.id))
        {
            return { error: `Duplicate datapoint id generated: ${dp.id}` };
        }
        seen.add(dp.id);
    }

    return {
        device: {
            name,
            busId,
            slaveId: slave,
            defaultByteOrder: byteOrder,
            dataPoints: dps || []
        }
    };
}

function addDeviceToJson() {
    let cfg;
    try {
        cfg = JSON.parse(elsCfg.jsonBox.value || '{}');
    } catch (e) {
        setStatus(elsCfg.status, 'Invalid JSON in editor. Fix it before adding device. ' + e.message, 'err');
        return;
    }

    const built = buildDeviceFromForm();
    if (built.error) {
        setStatus(elsCfg.status, built.error, 'err');
        return;
    }

    if (!Array.isArray(cfg.devices))
    {
        cfg.devices = [];
    }

    cfg.devices.push(built.device);

    if (!cfg.version) cfg.version = 1;
    if (!cfg.bus1) {
        cfg.bus1 = { baud: 9600, serialFormat: '8N1', termination: '120R', enableBias: true };
    }

    elsCfg.jsonBox.value = pretty(cfg);
    setStatus(elsCfg.status, `Device "${built.device.name}" added to JSON (bus ${built.device.busId}).`, 'ok');

    // reset form for next entry
    if (devEls.name)  devEls.name.value = '';
    if (devEls.slave) devEls.slave.value = '';
    if (devEls.order) devEls.order.value = 'ABCD';
    clearDpRows();
    addDpRow();
    updateAllDpIdPreviews();
}

/* ===========================
   Configure Network Page
   =========================== */

// === Network Config API ===
const API = {
    SSIDS: '/api/ssids',
    CONNECT: '/api/wifi/connect',
    STATUS: '/api/wifi/status',
    CANCEL: '/api/wifi/cancel' // optional
};

// Signal bars & badges
function rssiToBars(rssi) {
    // rssi ~ -30 excellent .. -90 bad
    const level = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
    return '📶'.repeat(level) + '·'.repeat(4 - level);
}
function rssiBadge(rssi) {
    const cls = rssi >= -60 ? 'ok' : rssi >= -72 ? 'warn' : 'bad';
    const label = rssi >= -60 ? 'Strong' : rssi >= -72 ? 'Fair' : 'Weak';
    return `<span class="badge ${cls}">${label}</span>`;
}
function isValidMacBssid(s) {
    return /^[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}$/.test(s.trim());
}
function isAnyFilled(...vals) { return vals.some(v => v && v.trim().length); }
function isValidIp(s) {
    if (!s) return false;
    const parts = s.split('.');
    if (parts.length !== 4) return false;
    return parts.every(p => /^\d+$/.test(p) && +p >= 0 && +p <= 255);
}

// DOM refs
const el = id => document.getElementById(id);
const list = el('list');
const filter = el('filter');
const refreshBtn = el('refresh');
const scanState = el('scanState');
const scanHint = el('scanHint');

const ssid = el('ssid');
const password = el('password');
const bssid = el('bssid');
const save = el('save');

const ip = el('ip'), gw = el('gw'), mask = el('mask'), dns1 = el('dns1'), dns2 = el('dns2');

const connectBtn = el('connect');
const cancelBtn = el('cancel');
const statusLog = el('statusLog');
const quickStatus = el('quickStatus');
const finalBadge = el('finalBadge');

const hiddenDetails = el('hiddenDetails');
const hiddenSsid = el('hiddenSsid');
const hiddenPass = el('hiddenPass');
const selectHidden = el('selectHidden');
const showHiddenPw = el('showHiddenPw');
const showPw = el('showPw');

// State
let networks = [];
let selectedIndex = -1;
let polling = null;
let connectDeadline = null;

// UI helpers
function setSelected(idx) {
    selectedIndex = idx;
    for (const item of list.querySelectorAll('.network')) item.classList.remove('selected');
    if (idx >= 0) list.querySelector(`[data-idx="${idx}"]`)?.classList.add('selected');
    const n = idx >= 0 ? networks[idx] : null;
    if (n) {
        ssid.value = n.ssid;
        bssid.value = n.bssid || '';
        // If secure, focus pw; if open, clear pw
        if (n.secure) password.focus(); else password.value = '';
    }
    finalBadge.innerHTML = '';
}

function renderList() {
    const q = filter.value.trim().toLowerCase();
    const items = networks
        .filter(n => !q || (n.ssid || '').toLowerCase().includes(q))
        .map((n, i) => {
            const secureHtml = n.secure
                ? `<span class="pill secure" title="${n.auth || 'Secured'}">🔒 ${n.auth || 'Secured'}</span>`
                : `<span class="pill" title="Open network">Open</span>`;
            return `
            <div class="network" role="listitem" data-idx="${i}">
              <div class="nw-main">
                <div class="ssid">${n.ssid || '<hidden>'} ${secureHtml}</div>
                <div class="meta">ch ${n.channel ?? '?'} • BSSID ${n.bssid || 'n/a'}</div>
              </div>
              <div class="rssi">${rssiToBars(n.rssi ?? -100)}<br/><span class="muted">${n.rssi ?? '–'} dBm</span><br/>${rssiBadge(n.rssi ?? -100)}</div>
            </div>`;
        }).join('');
    list.innerHTML = items || `<div class="muted">No networks found. Try <strong>Refresh</strong> or add a hidden SSID.</div>`;
    list.querySelectorAll('.network').forEach(div => {
        div.addEventListener('click', () => setSelected(+div.dataset.idx));
    });
}

async function fetchSSIDs() {
    scanState.innerHTML = `<span class="spinner"></span> Scanning…`;
    try {
        const res = await fetch(API.SSIDS, { cache: 'no-store' });
        if (!res.ok) throw new Error('Scan failed');
        const data = await res.json();
        networks = (data || []).map(m => ({
            ssid: m.ssid ?? m.SSID ?? '',
            rssi: m.rssi ?? m.RSSI ?? -100,
            secure: (m.secure ?? (m.auth && m.auth !== 'OPEN')) ?? false,
            auth: m.auth ?? (m.secure ? 'WPA/WPA2' : 'OPEN'),
            bssid: m.bssid ?? m.BSSID ?? '',
            channel: m.channel ?? m.chan ?? null
        }));
        networks.sort((a,b) => (b.rssi - a.rssi) || ((b.secure?1:0) - (a.secure?1:0)));
        renderList();
        scanState.textContent = `Found ${networks.length} network${networks.length===1?'':'s'}.`;
    } catch (e) {
        console.warn(e);
        scanState.innerHTML = `Scan failed. <span class="hint">Check logs; try again.</span>`;
    }
}

function getStaticBlock() {
    const hasStatic = isAnyFilled(ip.value, gw.value, mask.value, dns1.value, dns2.value);
    if (!hasStatic) return null;
    // minimal validation
    if (![ip.value, gw.value, mask.value].every(isValidIp)) {
        throw new Error('Static IP, gateway, and subnet must be valid IPv4 addresses.');
    }
    const obj = { ip: ip.value.trim(), gateway: gw.value.trim(), subnet: mask.value.trim() };
    if (isValidIp(dns1.value)) obj.dns1 = dns1.value.trim();
    if (isValidIp(dns2.value)) obj.dns2 = dns2.value.trim();
    return obj;
}

function log(msg) {
    statusLog.style.display = 'block';
    statusLog.textContent += (statusLog.textContent ? '\n' : '') + msg;
    statusLog.scrollTop = statusLog.scrollHeight;
}

async function connect() {
    finalBadge.innerHTML = '';
    quickStatus.textContent = '';
    statusLog.textContent = '';
    connectBtn.disabled = true;
    cancelBtn.disabled = false;
    log('Starting connection…');

    const payload = {
        ssid: ssid.value.trim(),
        password: password.value,
        bssid: bssid.value.trim() || undefined,
        channel: networks[selectedIndex]?.channel || 0, // <— add this line
        // static: st || undefined,
        save: (save.value === 'true')
    };
    if (!payload.ssid) {
        connectBtn.disabled = false; cancelBtn.disabled = true;
        return log('❌ Please select or enter an SSID first.');
    }
    const b = bssid.value.trim();
    if (b && !isValidMacBssid(b)) {
        connectBtn.disabled = false; cancelBtn.disabled = true;
        return log('❌ BSSID must look like AA:BB:CC:DD:EE:FF');
    }
    if (b) payload.bssid = b;

    try {
        const st = getStaticBlock();
        if (st) payload.static = st;
    } catch (e) {
        connectBtn.disabled = false; cancelBtn.disabled = true;
        return log('❌ ' + e.message);
    }

    try {
        log('→ POST /api/wifi/connect');
        const res = await fetch(API.CONNECT, {
            method: 'POST',
            headers: {'Content-Type':'application/json'},
            body: JSON.stringify(payload)
        });
        if (!res.ok) throw new Error(`Connect request failed (${res.status})`);
    } catch (e) {
        connectBtn.disabled = false; cancelBtn.disabled = true;
        return log('❌ ' + e.message);
    }

    // Poll status up to ~35s
    connectDeadline = Date.now() + 35000;
    if (polling) clearInterval(polling);
    polling = setInterval(checkStatus, 1000);
    quickStatus.innerHTML = `<span class="spinner"></span> Connecting…`;
}

async function checkStatus() {
    try {
        const res = await fetch(API.STATUS, { cache: 'no-store' });
        if (!res.ok) throw new Error('status http ' + res.status);
        const s = await res.json();
        const state = (s.state || '').toLowerCase();
        const ip = s.ip || '';
        if (state) log(`status: ${state}${ip ? ' ('+ip+')':''}`);
        if (state === 'connected') {
            done(true, ip);
        } else if (state === 'failed' || state === 'disconnected') {
            done(false, null, s.reason || 'unknown');
        } else {
            if (Date.now() > connectDeadline) {
                done(false, null, 'timeout');
            }
        }
    } catch (e) {
        log('warn: ' + e.message);
        if (Date.now() > connectDeadline) done(false, null, 'timeout');
    }
}

async function cancel() {
    if (polling) { clearInterval(polling); polling = null; }
    connectBtn.disabled = false; cancelBtn.disabled = true;
    quickStatus.textContent = 'Cancelled.';
    try {
        await fetch(API.CANCEL, { method: 'POST' });
    } catch (_) {}
    log('⏹️ Connect cancelled.');
}

function done(success, ipAddr, reason) {
    if (polling) { clearInterval(polling); polling = null; }
    connectBtn.disabled = false; cancelBtn.disabled = true;
    if (success) {
        quickStatus.textContent = 'Connected.';
        finalBadge.innerHTML = `<span class="badge ok">Connected ${ipAddr ? ' · '+ipAddr : ''}</span>`;
        log('✅ Connected.' + (ipAddr ? ' IP: ' + ipAddr : ''));
        setTimeout(() => { window.location.href = '/'; }, 1500);
    } else {
        quickStatus.textContent = 'Failed.';
        finalBadge.innerHTML = `<span class="badge bad">Failed${reason ? ' · '+reason : ''}</span>`;
        log('❌ Failed' + (reason ? ' ('+reason+')' : '') + '. Check password, AP proximity, or try static IP.');
    }
}

// Events
refreshBtn.addEventListener('click', fetchSSIDs);
filter.addEventListener('input', renderList);
connectBtn.addEventListener('click', connect);
cancelBtn.addEventListener('click', cancel);
showPw.addEventListener('change', () => { password.type = showPw.checked ? 'text' : 'password'; });
showHiddenPw.addEventListener('change', () => { hiddenPass.type = showHiddenPw.checked ? 'text' : 'password'; });

selectHidden.addEventListener('click', () => {
    if (!hiddenSsid.value.trim()) { hiddenSsid.focus(); return; }
    ssid.value = hiddenSsid.value.trim();
    password.value = hiddenPass.value;
    bssid.value = '';
    setSelected(-1);
    hiddenDetails.open = false;
    log('Using hidden SSID: ' + ssid.value);
});

document.addEventListener('DOMContentLoaded', fetchSSIDs);