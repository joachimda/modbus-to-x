import {API, STATIC_FILES, safeJson} from "app";

window.initConfigureMqtt = async function initConfigureMqtt() {
    await load();
    document.querySelector('#btn-save').onclick = saveWithStatus;
    document.querySelector('#btn-test').onclick = testConnectionWithStatus;
}

async function load() {
    try {
        const j = await safeJson(STATIC_FILES.MQTT_CONFIG_JSON);
        document.querySelector('#mqtt-enabled').checked = Boolean(j.enabled);
        document.querySelector('#broker-ip').value = j.broker_ip || '';
        document.querySelector('#broker-url').value = j.broker_url || '';
        document.querySelector('#broker-port').value = j.broker_port || '1883';
        document.querySelector('#broker-user').value = j.user || '';
        document.querySelector('#root-topic').value = j.root_topic || 'mbx_root';
    } catch (e) {
        alert('Failed to load MQTT config: ' + e.message);
    }
}

function setStatus(el, { pending, ok, error, text }) {
    if (!el) return;
    if (pending) {
        el.innerHTML = `<span class="spinner"></span> <span class="muted">${text || ''}</span>`;
        return;
    }
    if (ok) {
        el.innerHTML = `<span class="badge ok">\u2713 ${text || 'OK'}</span>`;
        return;
    }
    if (error) {
        el.innerHTML = `<span class="badge bad">Failed</span> <span class="muted">${text || ''}</span>`;
        return;
    }
    el.textContent = '';
}

async function testConnectionWithStatus() {
    const el = document.querySelector('#test-status');
    setStatus(el, { pending: true, text: 'Testing…' });
    try {
        const r = await safeJson(API.POST_MQTT_TEST, { method: 'POST' });
        if (r.ok) {
            setStatus(el, { ok: true, text: 'Connected' });
            setTimeout(() => { if (el) el.textContent=''; }, 2500);
        } else {
            const txt = `State=${r.state || 'n/a'}${r.error ? ' ('+r.error+')' : ''}`;
            setStatus(el, { error: true, text: txt });
            setTimeout(() => { if (el) el.textContent=''; }, 5000);
        }
    } catch (e) {
        setStatus(el, { error: true, text: e.message });
        setTimeout(() => { if (el) el.textContent=''; }, 5000);
    }
}

async function saveWithStatus() {
    const el = document.querySelector('#save-status');
    try {
        const cfg = readForm();
        const pass = (document.querySelector('#broker-pass').value || '').trim();
        setStatus(el, { pending: true, text: 'Saving…' });
        if (pass.length) {
            await safeJson(API.PUT_MQTT_SECRET, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ password: pass }),
            });
        }
        await safeJson(API.PUT_MQTT_CONFIG, {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(cfg),
        });
        if (pass.length) document.querySelector('#broker-pass').value = '';
        setStatus(el, { ok: true, text: 'Saved' });
        setTimeout(() => { if (el) el.textContent=''; }, 2500);
    } catch (e) {
        setStatus(el, { error: true, text: e.message });
        setTimeout(() => { if (el) el.textContent=''; }, 5000);
    }
}

/* async function testConnection() {
    try {
        const r = await safeJson(API.POST_MQTT_TEST, { method: 'POST' });
        const msg = r.ok
            ? `OK — Connected. State=${r.state}`
            : `Failed. State=${r.state || 'n/a'}${r.error ? ' ('+r.error+')' : ''}`;
        alert(`Test connection to ${r.broker || '(unknown)'} as ${r.user || '(anonymous)'}: ${msg}`);
    } catch (e) {
        alert('Test connection failed: ' + e.message);
    }
}
*/

function readForm() {
    const ip = (document.querySelector('#broker-ip').value || '').trim();
    const url = (document.querySelector('#broker-url').value || '').trim();
    const port = String((document.querySelector('#broker-port').value || '').trim() || '1883');
    const user = (document.querySelector('#broker-user').value || '').trim();
    const enabled = Boolean(document.querySelector('#mqtt-enabled').checked);
    let root_topic = (document.querySelector('#root-topic').value || '').trim();
    // Normalize and validate root topic
    // - trim leading/trailing slashes
    // - collapse multiple slashes
    // - disallow spaces and invalid chars
    root_topic = root_topic.replace(/^\/+|\/+$/g, '');
    root_topic = root_topic.replace(/\/{2,}/g, '/');
    if (!root_topic.length) root_topic = 'mbx_root';
    const valid = /^[A-Za-z0-9_\-]+(\/[A-Za-z0-9_\-]+)*$/.test(root_topic);
    if (!valid) {
        throw new Error('Root topic may contain letters, numbers, _ , - and / (as separators), no spaces.');
    }
    const pnum = Number(port);
    if (!Number.isInteger(pnum) || pnum < 1 || pnum > 65535) {
        throw new Error('Broker port must be 1-65535');
    }
    return {
        enabled,
        broker_ip: ip,
        broker_url: url,
        broker_port: port,
        user,
        root_topic,
    };
}

/* async function save() {
    try {
        const cfg = readForm();
        const pass = (document.querySelector('#broker-pass').value || '').trim();
        // Save secret first if provided; leaving blank keeps existing
        if (pass.length) {
            await safeJson(API.PUT_MQTT_SECRET, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ password: pass }),
            });
        }
        await safeJson(API.PUT_MQTT_CONFIG, {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(cfg),
        });
        if (pass.length) document.querySelector('#broker-pass').value = '';
        alert('Saved.');
    } catch (e) {
        alert('Save failed: ' + e.message);
    }
}
*/
