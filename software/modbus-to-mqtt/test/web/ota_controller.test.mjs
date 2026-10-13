import test from 'node:test';
import assert from 'node:assert/strict';

import {formatOtaSnapshot, OtaController} from '../../data/js/ota_controller.mjs';

const API = {
    OTA_SESSION: '/api/system/ota/session',
    OTA_STATUS: '/api/system/ota/status',
    OTA_EVENTS: '/api/system/ota/events',
    OTA_HTTP_CHECK: '/api/system/ota/http/check',
    OTA_HTTP_NOTES: '/api/system/ota/http/notes',
    OTA_HTTP_APPLY: '/api/system/ota/http/apply',
    OTA_MANUAL_START: '/api/system/ota/manual/start',
    OTA_PASSWORD: '/api/system/ota/password',
    OTA_FIRMWARE: '/api/system/ota/firmware',
    OTA_FILESYSTEM: '/api/system/ota/fs',
    FACTORY_RESET: '/api/system/factory-reset',
};

const TOKEN = '12'.repeat(32);

function jsonResponse(body, status = 200) {
    return new Response(JSON.stringify(body), {
        status,
        headers: {'Content-Type': 'application/json'},
    });
}

async function flush() {
    await Promise.resolve();
    await Promise.resolve();
    await new Promise(resolve => setImmediate(resolve));
}

class FakeEventSource {
    static instances = [];

    constructor(url) {
        this.url = url;
        this.readyState = 0;
        this.closed = false;
        this.listeners = new Map();
        FakeEventSource.instances.push(this);
    }

    addEventListener(name, callback) {
        this.listeners.set(name, callback);
    }

    emit(name, data = '') {
        if (name === 'open') this.readyState = 1;
        this.listeners.get(name)?.({data: typeof data === 'string' ? data : JSON.stringify(data)});
    }

    close() {
        this.closed = true;
        this.readyState = 2;
    }
}

test.beforeEach(() => {
    FakeEventSource.instances = [];
});

test('one password verification creates a RAM session reused by interactions', async () => {
    let sessionCalls = 0;
    let prompts = 0;
    const interactionHeaders = [];
    const controller = new OtaController({
        api: API,
        EventSourceCtor: null,
        promptPassword: () => {
            ++prompts;
            return 'route-password';
        },
        fetchFn: async (url, init) => {
            if (url === API.OTA_SESSION) {
                ++sessionCalls;
                if (!init.headers.get('Authorization')) return jsonResponse({error: 'unauthorized'}, 401);
                assert.match(init.headers.get('Authorization'), /^Bearer /);
                return jsonResponse({token: TOKEN, absoluteExpiresMs: 900000, idleExpiresMs: 300000});
            }
            interactionHeaders.push(init.headers);
            return jsonResponse({ok: true});
        },
    });

    await controller.requestJson(API.OTA_HTTP_CHECK, {method: 'POST'});
    await controller.requestJson(API.OTA_HTTP_NOTES, {method: 'POST'});

    assert.equal(prompts, 1);
    assert.equal(sessionCalls, 2);
    assert.equal(interactionHeaders.length, 2);
    for (const headers of interactionHeaders) {
        assert.equal(headers.get('X-OTA-Session'), TOKEN);
        assert.equal(headers.get('X-MBX-Request'), '1');
        assert.equal(headers.get('Authorization'), null);
    }
});

test('normal stream closes and stale, duplicate, and foreign snapshots are ignored', async () => {
    let normalCloses = 0;
    let normalResumes = 0;
    const snapshots = [];
    const controller = new OtaController({
        api: API,
        EventSourceCtor: FakeEventSource,
        closeNormalStream: () => ++normalCloses,
        resumeNormalStream: () => ++normalResumes,
        onSnapshot: snapshot => snapshots.push(snapshot.state),
        setTimeoutFn: () => 1,
        clearTimeoutFn: () => {},
        fetchFn: async url => {
            if (url === API.OTA_HTTP_APPLY) {
                return jsonResponse({ok: true, started: true, operationId: 7}, 202);
            }
            return jsonResponse({operationId: 7, sequence: 1, state: 'quiescing'});
        },
    });
    controller.sessionToken = TOKEN;

    await controller.apply();
    await flush();
    const source = FakeEventSource.instances[0];
    source.emit('ota-progress', {operationId: 7, sequence: 2, state: 'downloading_writing_firmware'});
    source.emit('ota-progress', {operationId: 7, sequence: 2, state: 'verifying_firmware'});
    source.emit('ota-progress', {operationId: 6, sequence: 99, state: 'rebooting'});
    source.emit('ota-progress', {operationId: 7, sequence: 3, state: 'verifying_firmware'});
    source.emit('ota-progress', {operationId: 7, sequence: 4, state: 'failed', error: 'write_failed'});

    assert.equal(normalCloses, 1);
    assert.deepEqual(snapshots, [
        'quiescing',
        'downloading_writing_firmware',
        'verifying_firmware',
        'failed',
    ]);
    assert.equal(normalResumes, 1);
    assert.equal(controller.operationId, 0);
    assert.equal(source.closed, true);
});

test('SSE failure falls back to single-flight status polling and retries SSE', async () => {
    const timers = [];
    const transports = [];
    let statusCalls = 0;
    const controller = new OtaController({
        api: API,
        EventSourceCtor: FakeEventSource,
        onTransport: mode => transports.push(mode),
        setTimeoutFn: callback => {
            timers.push(callback);
            return timers.length;
        },
        clearTimeoutFn: () => {},
        fetchFn: async () => {
            ++statusCalls;
            return jsonResponse({operationId: 9, sequence: statusCalls, state: 'quiescing'});
        },
    });

    controller.monitor(9);
    await flush();
    const first = FakeEventSource.instances[0];
    first.emit('open');
    first.emit('error');
    await flush();

    assert.equal(first.closed, true);
    assert.ok(statusCalls >= 2);
    assert.equal(transports.at(-1), 'polling');
    const retry = timers[0];
    retry();
    assert.equal(FakeEventSource.instances.length, 2);
});

test('a lost apply response consults public status and continues the admitted operation', async () => {
    const snapshots = [];
    let applyCalls = 0;
    const controller = new OtaController({
        api: API,
        EventSourceCtor: FakeEventSource,
        setTimeoutFn: () => 1,
        clearTimeoutFn: () => {},
        onSnapshot: snapshot => snapshots.push(snapshot.state),
        fetchFn: async url => {
            if (url === API.OTA_HTTP_APPLY) {
                ++applyCalls;
                throw new TypeError('connection closed');
            }
            return jsonResponse({operationId: 12, sequence: 4, state: 'quiescing'});
        },
    });
    controller.sessionToken = TOKEN;

    const result = await controller.apply();
    await flush();

    assert.equal(applyCalls, 1);
    assert.equal(result.recovered, true);
    assert.equal(controller.operationId, 12);
    assert.deepEqual(snapshots, ['quiescing']);
});

test('manual upload waits for awaiting-upload and binds session and operation headers', async () => {
    const calls = [];
    const file = new Blob(['firmware-image']);
    Object.defineProperty(file, 'name', {value: 'firmware.bin'});
    const controller = new OtaController({
        api: API,
        EventSourceCtor: FakeEventSource,
        setTimeoutFn: () => 1,
        clearTimeoutFn: () => {},
        fetchFn: async (url, init) => {
            calls.push([url, init]);
            if (url === API.OTA_MANUAL_START) {
                assert.deepEqual(JSON.parse(init.body), {component: 'firmware', size: file.size});
                return jsonResponse({ok: true, operationId: 23}, 202);
            }
            if (url === API.OTA_STATUS) {
                return jsonResponse({operationId: 23, sequence: 2, state: 'awaiting_upload'});
            }
            if (url === API.OTA_FIRMWARE) return jsonResponse({ok: true});
            throw new Error(`unexpected URL ${url}`);
        },
    });
    controller.sessionToken = TOKEN;

    const result = await controller.manualUpload('firmware', file);
    assert.equal(result.ok, true);
    const [, upload] = calls.find(([url]) => url === API.OTA_FIRMWARE);
    assert.equal(upload.headers.get('X-OTA-Session'), TOKEN);
    assert.equal(upload.headers.get('X-OTA-Operation'), '23');
    assert.equal(upload.headers.get('X-MBX-Request'), '1');
});

test('a lost manual preflight response recovers the operation and still uploads', async () => {
    const file = new Blob(['filesystem-image']);
    Object.defineProperty(file, 'name', {value: 'filesystem.bin'});
    let statusSequence = 0;
    let uploadCalls = 0;
    const controller = new OtaController({
        api: API,
        EventSourceCtor: null,
        setTimeoutFn: () => 1,
        clearTimeoutFn: () => {},
        fetchFn: async (url, init) => {
            if (url === API.OTA_MANUAL_START) throw new TypeError('response lost');
            if (url === API.OTA_STATUS) {
                return jsonResponse({
                    operationId: 31,
                    sequence: ++statusSequence,
                    state: 'awaiting_upload',
                });
            }
            if (url === API.OTA_FILESYSTEM) {
                ++uploadCalls;
                assert.equal(init.headers.get('X-OTA-Operation'), '31');
                return jsonResponse({ok: true});
            }
            throw new Error(`unexpected URL ${url}`);
        },
    });
    controller.sessionToken = TOKEN;

    const result = await controller.manualUpload('filesystem', file);
    assert.equal(result.ok, true);
    assert.equal(uploadCalls, 1);
});

test('snapshot formatting distinguishes stages, bytes, component, overall, and failures', () => {
    assert.equal(formatOtaSnapshot({
        state: 'downloading_writing_filesystem',
        targetVersion: '2.4.0',
        component: 'filesystem',
        receivedBytes: 512,
        totalBytes: 1024,
        componentPercentKnown: true,
        componentPercent: 50,
        overallPercentKnown: true,
        overallPercent: 80,
    }, value => `${value} bytes`),
    'Downloading and writing filesystem to 2.4.0 — 512 bytes / 1024 bytes (50%) — overall 80%');
    assert.equal(formatOtaSnapshot({state: 'failed', error: 'ui_remount_failed'}),
                 'Update failed: ui_remount_failed');
});
