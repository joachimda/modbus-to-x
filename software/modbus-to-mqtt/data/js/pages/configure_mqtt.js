import {API, STATIC_FILES, safeJson} from "app";

let mqttConstraints = null;

window.initConfigureMqtt = async function initConfigureMqtt() {
    try {
        mqttConstraints = await safeJson(API.GET_MQTT_CONSTRAINTS);
        applyConstraints();
    } catch (e) {
        document.querySelector('#btn-save').disabled = true;
        document.querySelector('#btn-test').disabled = true;
        alert('Failed to load MQTT field constraints: ' + e.message);
        return;
    }
    await load();
    document.querySelector('#btn-save').onclick = saveWithStatus;
    document.querySelector('#btn-test').onclick = testConnectionWithStatus;
}

function applyConstraints() {
    document.querySelector('#broker-ip').maxLength = mqttConstraints.brokerIpMaxBytes;
    document.querySelector('#broker-user').maxLength = mqttConstraints.userMaxBytes;
    document.querySelector('#broker-pass').maxLength = mqttConstraints.passwordMaxBytes;

    const port = document.querySelector('#broker-port');
    port.minLength = 1;
    port.maxLength = mqttConstraints.portMaxBytes;

    const hint = document.querySelector('#mqtt-constraints-hint');
    hint.textContent = `Broker IP and URL host: ${mqttConstraints.brokerMaxBytes} UTF-8 bytes maximum. `
        + `Port: ${mqttConstraints.portMin}-${mqttConstraints.portMax}. `
        + `Username: ${mqttConstraints.userMaxBytes} UTF-8 bytes maximum. `
        + `Password: ${mqttConstraints.passwordMaxBytes} UTF-8 bytes maximum.`;
}

async function load() {
    try {
        const j = await safeJson(STATIC_FILES.MQTT_CONFIG_JSON);
        document.querySelector('#mqtt-enabled').checked = Boolean(j.enabled);
        document.querySelector('#broker-ip').value = j.broker_ip || '';
        document.querySelector('#broker-url').value = j.broker_url || '';
        document.querySelector('#broker-port').value = j.broker_port === undefined || j.broker_port === null
            ? '1883'
            : String(j.broker_port);
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
        const pass = document.querySelector('#broker-pass').value || '';
        validateMaximumBytes('Password', pass, mqttConstraints.passwordMaxBytes);
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
    if (!mqttConstraints) throw new Error('MQTT field constraints are unavailable.');
    const ip = (document.querySelector('#broker-ip').value || '').trim();
    const url = (document.querySelector('#broker-url').value || '').trim();
    const port = String((document.querySelector('#broker-port').value || '').trim());
    const user = (document.querySelector('#broker-user').value || '').trim();
    const enabled = Boolean(document.querySelector('#mqtt-enabled').checked);

    validateMaximumBytes('Broker IP', ip, mqttConstraints.brokerIpMaxBytes);
    const urlHost = extractHost(url);
    validateMaximumBytes('Broker URL host', urlHost, mqttConstraints.brokerUrlHostMaxBytes);
    const selectedBroker = ip.length && ip !== '0.0.0.0'
        ? ip
        : (url.length ? urlHost : '0.0.0.0');
    validateMaximumBytes('Selected broker', selectedBroker, mqttConstraints.brokerMaxBytes);
    validateMaximumBytes('Username', user, mqttConstraints.userMaxBytes);

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
    if (!/^[0-9]+$/.test(port) || utf8Length(port) > mqttConstraints.portMaxBytes) {
        throw new Error(`Broker port must contain 1-${mqttConstraints.portMaxBytes} ASCII decimal digits.`);
    }
    const pnum = Number(port);
    if (!Number.isInteger(pnum) || pnum < mqttConstraints.portMin || pnum > mqttConstraints.portMax) {
        throw new Error(`Broker port must be ${mqttConstraints.portMin}-${mqttConstraints.portMax}.`);
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

function utf8Length(value) {
    return new TextEncoder().encode(value).length;
}

function validateMaximumBytes(label, value, maximum) {
    const length = utf8Length(value);
    if (length > maximum) {
        throw new Error(`${label} must be at most ${maximum} UTF-8 bytes; received ${length}.`);
    }
}

function extractHost(url) {
    if (!url.length) return '';
    const scheme = url.indexOf('://');
    const start = scheme >= 0 ? scheme + 3 : 0;
    const slash = url.indexOf('/', start);
    const colon = url.indexOf(':', start);
    let end = url.length;
    if (slash >= 0 && slash < end) end = slash;
    if (colon >= 0 && colon < end) end = colon;
    return url.slice(start, end);
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
