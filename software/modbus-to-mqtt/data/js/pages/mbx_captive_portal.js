import {API, mbxFetch, reboot, rssiBadge, rssiToBars} from 'app';
import {ProvisioningHandoff} from './handoff.mjs';

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
const handoffActions = el('handoffActions');
const retryCompletionBtn = el('retryCompletion');
const retryReadinessBtn = el('retryReadiness');
const dashboardLink = el('dashboardLink');

const hiddenDetails = el('hiddenDetails');
const hiddenSsid = el('hiddenSsid');
const hiddenPass = el('hiddenPass');
const selectHidden = el('selectHidden');
const showHiddenPw = el('showHiddenPw');
const showPw = el('showPw');

let networks = [];
let selectedIndex = -1;
const handoff = new ProvisioningHandoff({
    fetchFn: (...args) => fetch(...args),
    mutationFetchFn: (...args) => mbxFetch(...args),
    statusUrl: API.STATUS,
    completionUrl: API.COMPLETE_PROVISIONING,
    onState: renderHandoffState,
});

function hideHandoffActions() {
    handoffActions.hidden = true;
    retryCompletionBtn.hidden = true;
    retryReadinessBtn.hidden = true;
    dashboardLink.hidden = true;
    dashboardLink.removeAttribute('href');
}

function showDashboardAction(dashboardUrl) {
    handoffActions.hidden = false;
    dashboardLink.hidden = false;
    dashboardLink.href = dashboardUrl;
}

function friendlyReason(reason) {
    const messages = {
        PERSISTENCE_REQUIRED: 'This was a temporary connection. Select “Persist” to finish setup.',
        CREDENTIAL_PERSIST_FAILED: 'The device connected, but saving the credentials failed.',
        NETWORK_CONFIG_PERSIST_FAILED: 'The device connected, but saving the DHCP/static configuration failed.',
    };
    return messages[reason] || reason || 'Provisioning is not ready to finish.';
}

function stationNetworkGuidance() {
    const targetSsid = ssid.value.trim();
    return targetSsid
        ? `Reconnect this browser to “${targetSsid}” if needed.`
        : 'Reconnect this browser to the configured Wi-Fi network if needed.';
}

function renderHandoffState(state) {
    switch (state.phase) {
        case 'connecting':
            quickStatus.innerHTML = `<span class="spinner"></span> Connecting…`;
            break;
        case 'status':
            if (state.state) log(`status: ${state.state}${state.ip ? ` (${state.ip})` : ''}`);
            break;
        case 'status_warning':
            log(`warn: ${state.error}`);
            break;
        case 'ready_for_completion':
            quickStatus.textContent = 'Connected. Finishing setup…';
            finalBadge.innerHTML = `<span class="badge ok">Connected · ${state.ip}</span>`;
            log(`✅ Connected with persisted settings. Station IP: ${state.ip}`);
            break;
        case 'completion_requesting':
            quickStatus.innerHTML = `<span class="spinner"></span> Requesting controlled reboot…`;
            retryCompletionBtn.disabled = true;
            log('→ POST /api/wifi/ap_off');
            break;
        case 'connected_not_ready':
            connectBtn.disabled = false;
            cancelBtn.disabled = true;
            quickStatus.textContent = 'Connected, but setup is not ready to finish.';
            finalBadge.innerHTML = `<span class="badge warn">Connected · not saved</span>`;
            log(`⚠️ ${friendlyReason(state.reason)}`);
            break;
        case 'connection_failed':
            connectBtn.disabled = false;
            cancelBtn.disabled = true;
            quickStatus.textContent = 'Connection failed.';
            finalBadge.innerHTML = `<span class="badge bad">Failed · ${state.reason}</span>`;
            log(`❌ Connection failed (${state.reason}). The setup portal remains available.`);
            break;
        case 'connection_timeout':
            connectBtn.disabled = false;
            cancelBtn.disabled = true;
            quickStatus.textContent = 'Connection timed out.';
            finalBadge.innerHTML = `<span class="badge bad">Timed out</span>`;
            log('❌ Connection timed out. The setup portal remains available.');
            break;
        case 'completion_rejected':
        case 'completion_unavailable':
            connectBtn.disabled = false;
            cancelBtn.disabled = true;
            handoffActions.hidden = false;
            retryCompletionBtn.hidden = false;
            retryCompletionBtn.disabled = false;
            quickStatus.textContent = state.phase === 'completion_rejected'
                ? 'Device reports provisioning is not ready.'
                : 'The reboot could not be scheduled.';
            log(state.phase === 'completion_rejected'
                ? '⚠️ Completion was rejected as not ready. Retry explicitly or start another saved attempt.'
                : '❌ Reboot scheduling is unavailable. No reboot was scheduled.');
            break;
        case 'completion_invalid':
            connectBtn.disabled = false;
            cancelBtn.disabled = true;
            quickStatus.textContent = 'The completion response could not be verified.';
            log('⚠️ Completion returned an unexpected or malformed response; setup is not being reported as complete.');
            break;
        case 'reboot_accepted':
            connectBtn.disabled = true;
            cancelBtn.disabled = true;
            quickStatus.textContent = `Reboot accepted. ${stationNetworkGuidance()}`;
            log(`✅ Reboot accepted for station address ${state.ip}.`);
            log(`ℹ️ ${stationNetworkGuidance()} The device cannot move this browser between Wi-Fi networks.`);
            break;
        case 'reboot_pending':
            connectBtn.disabled = true;
            cancelBtn.disabled = true;
            quickStatus.textContent = 'A reboot is already pending. Waiting for station mode…';
            log('ℹ️ The device reports that a reboot is already pending.');
            break;
        case 'waiting_for_station':
            quickStatus.innerHTML = state.retry
                ? `<span class="spinner"></span> Checking ${state.ip} once…`
                : `<span class="spinner"></span> Waiting for ${state.ip}…`;
            dashboardLink.href = state.dashboardUrl;
            break;
        case 'readiness_wait':
            quickStatus.textContent = state.retry
                ? `The captive browser has not verified ${state.ip}.`
                : `Waiting for an exact readiness response from ${state.ip}…`;
            break;
        case 'readiness_timeout':
            handoffActions.hidden = false;
            retryReadinessBtn.hidden = false;
            retryReadinessBtn.disabled = false;
            showDashboardAction(state.dashboardUrl);
            quickStatus.textContent = state.retry
                ? 'This captive browser could not verify readiness. Use “Open dashboard”.'
                : `${stationNetworkGuidance()} Then retry the readiness check.`;
            finalBadge.innerHTML = `<span class="badge warn">Station not confirmed · ${state.ip}</span>`;
            if (state.retry) {
                log(`⚠️ The explicit readiness request to ${state.ip} did not return the exact response.`);
                log('ℹ️ This captive browser may block cross-network requests; use “Open dashboard”.');
            } else {
                log(`⚠️ No exact station-readiness response was received from ${state.ip} within 90 seconds.`);
                log(`ℹ️ ${stationNetworkGuidance()} Then use “Retry readiness check” or “Open dashboard”.`);
            }
            break;
        case 'station_ready':
            quickStatus.textContent = 'Station mode is ready. Opening dashboard…';
            finalBadge.innerHTML = `<span class="badge ok">Ready · ${state.ip}</span>`;
            log(`✅ Station readiness confirmed at ${state.ip}. Opening the dashboard.`);
            window.location.assign(state.dashboardUrl);
            break;
        case 'cancelled':
            quickStatus.textContent = 'Cancelled.';
            break;
    }
}

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
    if ((dns1.value.trim() && !isValidIp(dns1.value.trim()))
        || (dns2.value.trim() && !isValidIp(dns2.value.trim()))) {
        throw new Error('DNS values must be valid IPv4 addresses when provided.');
    }
    const obj = { ip: ip.value.trim(), gateway: gw.value.trim(), subnet: mask.value.trim() };
    if (dns1.value.trim()) obj.dns1 = dns1.value.trim();
    if (dns2.value.trim()) obj.dns2 = dns2.value.trim();
    return obj;
}

function log(msg) {
    statusLog.style.display = 'block';
    statusLog.textContent += (statusLog.textContent ? '\n' : '') + msg;
    statusLog.scrollTop = statusLog.scrollHeight;
}

async function connect() {
    handoff.cancel(false);
    hideHandoffActions();
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
        const res = await mbxFetch(API.CONNECT, {
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

    void handoff.beginAttempt(35000);
}

async function cancel() {
    handoff.cancel();
    connectBtn.disabled = false; cancelBtn.disabled = true;
    quickStatus.textContent = 'Cancelled.';
    try {
        await mbxFetch(API.CANCEL, { method: 'POST' });
    } catch (_) {}
    log('⏹️ Connect cancelled.');
}

refreshBtn.addEventListener('click', fetchSSIDs);
filter.addEventListener('input', renderList);
connectBtn.addEventListener('click', connect);
cancelBtn.addEventListener('click', cancel);
retryCompletionBtn.addEventListener('click', async () => {
    retryCompletionBtn.disabled = true;
    await handoff.retryCompletion();
    retryCompletionBtn.disabled = false;
});
retryReadinessBtn.addEventListener('click', async () => {
    retryReadinessBtn.disabled = true;
    await handoff.retryReadiness();
    retryReadinessBtn.disabled = false;
});
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
