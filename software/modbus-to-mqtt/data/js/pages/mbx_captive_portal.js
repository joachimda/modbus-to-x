import {API, reboot, rssiBadge, rssiToBars} from 'app';

window.initCaptivePortal = async function initCaptivePortal() {
    document.querySelector('#btn-reboot').onclick = reboot;
    await fetchSSIDs();
}

function isValidMacBssid(s) {
    return /^[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}$/.test(s.trim());
}
function isAnyFilled(...vals) {
    return vals.some(v => v && v.trim().length);
}

function isValidIp(s) {
    if (!s) return false;
    const parts = s.split('.');
    if (parts.length !== 4) return false;
    return parts.every(p => /^\d+$/.test(p) && +p >= 0 && +p <= 255);
}

const el = id => document.getElementById(id);
const list = el('list');
const filter = el('filter');
const refreshBtn = el('refresh');
const scanState = el('scanState');
el('scanHint');
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

let networks = [];
let selectedIndex = -1;
let polling = null;
let connectDeadline = null;

function setSelected(idx) {
    selectedIndex = idx;
    for (const item of list.querySelectorAll('.network')) item.classList.remove('selected');
    if (idx >= 0) list.querySelector(`[data-idx="${idx}"]`)?.classList.add('selected');
    const n = idx >= 0 ? networks[idx] : null;
    if (n) {
        ssid.value = n.ssid;
        bssid.value = n.bssid || '';
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
        if (!res.ok) {
            throw new Error('Scan failed');
        }
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
        channel: networks[selectedIndex]?.channel || 0,
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
        if (st) {
            payload.static = st;
        }
    }
    catch (e) {
        connectBtn.disabled = false;
        cancelBtn.disabled = true;
        return log('❌ ' + e.message);
    }

    try {
        log('→ POST /api/wifi/connect');
        const res = await fetch(API.CONNECT, {
            method: 'POST',
            headers: {'Content-Type':'application/json'},
            body: JSON.stringify(payload)
        });
        if (!res.ok) {
            throw new Error(`Connect request failed (${res.status})`);
        }
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
        if (!res.ok) {
            throw new Error('status http ' + res.status);
        }
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
    }
    catch (e) {
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

refreshBtn.addEventListener('click', fetchSSIDs);
filter.addEventListener('input', renderList);
connectBtn.addEventListener('click', connect);
cancelBtn.addEventListener('click', cancel);
showPw.addEventListener('change', () => {
    password.type = showPw.checked ? 'text' : 'password';
});

showHiddenPw.addEventListener('change', () => {
    hiddenPass.type = showHiddenPw.checked ? 'text' : 'password';
});

selectHidden.addEventListener('click', () => {
    if (!hiddenSsid.value.trim()) {
        hiddenSsid.focus();
        return;
    }
    ssid.value = hiddenSsid.value.trim();
    password.value = hiddenPass.value;
    bssid.value = '';
    setSelected(-1);
    hiddenDetails.open = false;
    log('Using hidden SSID: ' + ssid.value);
});

