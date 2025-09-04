import {API, STATIC_FILES, safeJson} from "app";

window.initConfigureMqtt = async function initConfigureMqtt() {
    await load();
    document.querySelector('#btn-reload').onclick = load;
    document.querySelector('#btn-save').onclick = save;
    document.querySelector('#btn-save-pass').onclick = savePassword;
    document.querySelector('#btn-test').onclick = testConnection;
}

async function load() {
    try {
        const j = await safeJson(STATIC_FILES.MQTT_CONFIG_JSON);
        document.querySelector('#broker-ip').value = j.broker_ip || '';
        document.querySelector('#broker-url').value = j.broker_url || '';
        document.querySelector('#broker-port').value = j.broker_port || '1883';
        document.querySelector('#broker-user').value = j.user || '';
    } catch (e) {
        alert('Failed to load MQTT config: ' + e.message);
    }
}

async function testConnection() {
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

function readForm() {
    const ip = (document.querySelector('#broker-ip').value || '').trim();
    const url = (document.querySelector('#broker-url').value || '').trim();
    const port = String((document.querySelector('#broker-port').value || '').trim() || '1883');
    const user = (document.querySelector('#broker-user').value || '').trim();
    const pnum = Number(port);
    if (!Number.isInteger(pnum) || pnum < 1 || pnum > 65535) {
        throw new Error('Broker port must be 1-65535');
    }
    return {
        broker_ip: ip,
        broker_url: url,
        broker_port: port,
        user,
    };
}

async function save() {
    try {
        const cfg = readForm();
        await safeJson(API.PUT_MQTT_CONFIG, {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(cfg),
        });
        alert('Saved.');
    } catch (e) {
        alert('Save failed: ' + e.message);
    }
}

async function savePassword() {
    const pass = (document.querySelector('#broker-pass').value || '').trim();
    try {
        await safeJson(API.PUT_MQTT_SECRET, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ password: pass }),
        });
        alert('Password saved.');
        document.querySelector('#broker-pass').value = '';
    } catch (e) {
        alert('Saving password failed: ' + e.message);
    }
}
