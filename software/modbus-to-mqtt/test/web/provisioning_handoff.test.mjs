import test from 'node:test';
import assert from 'node:assert/strict';

import {isIpv4Address, ProvisioningHandoff} from '../../data/js/pages/handoff.mjs';

function jsonResponse(body, status = 200) {
    return new Response(JSON.stringify(body), {
        status,
        headers: {'Content-Type': 'application/json'},
    });
}

function htmlResponse(body = '<html>captive</html>', status = 200) {
    return new Response(body, {status, headers: {'Content-Type': 'text/html'}});
}

function malformedJsonResponse(status = 200) {
    return new Response('{"ok":', {status, headers: {'Content-Type': 'application/json'}});
}

test('status polling is single-flight and automatically completes exactly once', async () => {
    let statusCalls = 0;
    let activeStatusCalls = 0;
    let maximumActiveStatusCalls = 0;
    let completionCalls = 0;
    const phases = [];
    const handoff = new ProvisioningHandoff({
        fetchFn: async url => {
            if (url === '/api/wifi/status') {
                ++statusCalls;
                ++activeStatusCalls;
                maximumActiveStatusCalls = Math.max(maximumActiveStatusCalls, activeStatusCalls);
                await Promise.resolve();
                --activeStatusCalls;
                return statusCalls === 1
                    ? jsonResponse({state: 'connecting', provisioningReady: false})
                    : jsonResponse({state: 'connected', ip: '192.168.20.72', provisioningReady: true});
            }
            return jsonResponse({ok: true, mode: 'station'});
        },
        mutationFetchFn: async () => {
            ++completionCalls;
            return jsonResponse({ok: true, rebooting: true, ip: '192.168.20.72'}, 202);
        },
        sleepFn: async () => {},
        onState: state => phases.push(state.phase),
    });

    assert.equal(await handoff.beginAttempt(), true);
    assert.equal(maximumActiveStatusCalls, 1);
    assert.equal(statusCalls, 2);
    assert.equal(completionCalls, 1);
    assert.equal(phases.filter(phase => phase === 'completion_requesting').length, 1);
    assert.equal(phases.at(-1), 'station_ready');
});

test('an older out-of-order status response cannot complete a newer attempt', async () => {
    let resolveOldStatus;
    let statusCalls = 0;
    let completionCalls = 0;
    const oldStatus = new Promise(resolve => { resolveOldStatus = resolve; });
    const handoff = new ProvisioningHandoff({
        fetchFn: async url => {
            if (url !== '/api/wifi/status') return jsonResponse({ok: true, mode: 'station'});
            ++statusCalls;
            if (statusCalls === 1) return oldStatus;
            return jsonResponse({state: 'connected', ip: '192.168.20.73', provisioningReady: true});
        },
        mutationFetchFn: async () => {
            ++completionCalls;
            return jsonResponse({ok: true, rebooting: true, ip: '192.168.20.73'}, 202);
        },
        sleepFn: async () => {},
    });

    const oldAttempt = handoff.beginAttempt();
    await Promise.resolve();
    const currentAttempt = handoff.beginAttempt();
    assert.equal(await currentAttempt, true);
    resolveOldStatus(jsonResponse({state: 'connected', ip: '192.168.20.90', provisioningReady: true}));
    assert.equal(await oldAttempt, false);
    assert.equal(completionCalls, 1);
    assert.equal(handoff.stationIp, '192.168.20.73');
});

test('temporary and persistence-failed connections never auto-complete', async () => {
    for (const reason of ['PERSISTENCE_REQUIRED', 'CREDENTIAL_PERSIST_FAILED', 'NETWORK_CONFIG_PERSIST_FAILED']) {
        let completionCalls = 0;
        const phases = [];
        const handoff = new ProvisioningHandoff({
            fetchFn: async () => jsonResponse({
                state: 'connected',
                ip: '192.168.20.72',
                provisioningReady: false,
                reason,
            }),
            mutationFetchFn: async () => {
                ++completionCalls;
                return jsonResponse({}, 500);
            },
            onState: state => phases.push(state),
        });
        assert.equal(await handoff.beginAttempt(), false);
        assert.equal(completionCalls, 0);
        assert.equal(phases.at(-1).phase, 'connected_not_ready');
        assert.equal(phases.at(-1).reason, reason);
    }
});

test('zero and malformed station addresses never reach completion or readiness', async () => {
    const invalidAddresses = [
        '', '0.0.0.0', '192.168.1', '192.168.1.1.2', '192.168.1.256',
        '192.168.01.2', '192.168.-1.2', 'not-an-address',
    ];
    for (const ip of invalidAddresses) {
        assert.equal(isIpv4Address(ip), false, ip);
        let completionCalls = 0;
        const phases = [];
        const handoff = new ProvisioningHandoff({
            fetchFn: async () => jsonResponse({state: 'connected', ip, provisioningReady: true}),
            mutationFetchFn: async () => {
                ++completionCalls;
                return jsonResponse({ok: true, rebooting: true, ip}, 202);
            },
            onState: state => phases.push(state.phase),
        });
        assert.equal(await handoff.beginAttempt(), false);
        assert.equal(completionCalls, 0);
        assert.equal(phases.at(-1), 'connected_not_ready');
    }
    assert.equal(isIpv4Address('192.168.20.72'), true);
});

test('completion response contracts remain distinct', async () => {
    const cases = [
        [jsonResponse({ok: false, error: 'provisioning_not_ready'}, 409), 'completion_rejected'],
        [jsonResponse({ok: false, error: 'reboot_unavailable'}, 503), 'completion_unavailable'],
        [htmlResponse(), 'completion_invalid'],
        [malformedJsonResponse(202), 'completion_invalid'],
        [jsonResponse({ok: true, rebooting: true}, 202), 'completion_invalid'],
    ];
    for (const [completionResponse, expectedPhase] of cases) {
        const phases = [];
        const handoff = new ProvisioningHandoff({
            fetchFn: async () => jsonResponse({
                state: 'connected', ip: '192.168.20.72', provisioningReady: true,
            }),
            mutationFetchFn: async () => completionResponse,
            onState: state => phases.push(state.phase),
        });
        assert.equal(await handoff.beginAttempt(), false);
        assert.equal(phases.at(-1), expectedPhase);
    }
});

test('reboot_pending waits for readiness without claiming a new reboot', async () => {
    let completionCalls = 0;
    const phases = [];
    const handoff = new ProvisioningHandoff({
        fetchFn: async url => url === '/api/wifi/status'
            ? jsonResponse({state: 'connected', ip: '192.168.20.72', provisioningReady: true})
            : jsonResponse({ok: true, mode: 'station'}),
        mutationFetchFn: async () => {
            ++completionCalls;
            return jsonResponse({ok: false, error: 'reboot_pending'}, 409);
        },
        onState: state => phases.push(state.phase),
    });
    assert.equal(await handoff.beginAttempt(), true);
    assert.equal(completionCalls, 1);
    assert.ok(phases.includes('reboot_pending'));
    assert.equal(phases.at(-1), 'station_ready');
});

test('readiness rejects captive, malformed, redirected, and unrelated responses', async () => {
    let readinessCall = 0;
    let now = 0;
    const phases = [];
    const invalidResponses = [
        htmlResponse(),
        malformedJsonResponse(),
        {status: 0, type: 'opaque', redirected: false, headers: new Headers(), json: async () => ({})},
        jsonResponse({ok: true, mode: 'station', extra: true}),
        {status: 200, type: 'basic', redirected: true, headers: new Headers({'Content-Type': 'application/json'}),
            json: async () => ({ok: true, mode: 'station'})},
        jsonResponse({ok: true, mode: 'portal'}),
    ];
    const handoff = new ProvisioningHandoff({
        fetchFn: async url => {
            if (url === '/api/wifi/status') {
                return jsonResponse({state: 'connected', ip: '192.168.20.72', provisioningReady: true});
            }
            const response = invalidResponses[readinessCall];
            ++readinessCall;
            return response || jsonResponse({ok: true, mode: 'station'});
        },
        mutationFetchFn: async () => jsonResponse({ok: true, rebooting: true, ip: '192.168.20.72'}, 202),
        nowFn: () => now,
        sleepFn: async milliseconds => { now += milliseconds; },
        onState: state => phases.push(state.phase),
    });
    assert.equal(await handoff.beginAttempt(), true);
    assert.equal(readinessCall, invalidResponses.length + 1);
    assert.equal(phases.filter(phase => phase === 'readiness_wait').length, invalidResponses.length);
    assert.equal(phases.at(-1), 'station_ready');
});

test('readiness timeout retains the address and explicit retry does not repeat completion', async () => {
    let now = 0;
    let ready = false;
    let completionCalls = 0;
    const states = [];
    const handoff = new ProvisioningHandoff({
        fetchFn: async url => {
            if (url === '/api/wifi/status') {
                return jsonResponse({state: 'connected', ip: '192.168.20.72', provisioningReady: true});
            }
            return ready ? jsonResponse({ok: true, mode: 'station'}) : htmlResponse();
        },
        mutationFetchFn: async () => {
            ++completionCalls;
            return jsonResponse({ok: true, rebooting: true, ip: '192.168.20.72'}, 202);
        },
        nowFn: () => now,
        sleepFn: async milliseconds => { now += milliseconds; },
        readinessDeadlineMs: 3000,
        readinessPollMs: 1000,
        onState: state => states.push(state),
    });
    assert.equal(await handoff.beginAttempt(), false);
    const timeout = states.at(-1);
    assert.equal(timeout.phase, 'readiness_timeout');
    assert.equal(timeout.ip, '192.168.20.72');
    assert.equal(timeout.dashboardUrl, 'http://192.168.20.72/');

    ready = true;
    assert.equal(await handoff.retryReadiness(), true);
    assert.equal(completionCalls, 1);
    assert.equal(states.at(-1).phase, 'station_ready');
});

test('an unsuccessful explicit readiness retry makes only one bounded request', async () => {
    let now = 0;
    let readinessCalls = 0;
    const states = [];
    const handoff = new ProvisioningHandoff({
        fetchFn: async url => {
            if (url === '/api/wifi/status') {
                return jsonResponse({state: 'connected', ip: '192.168.20.72', provisioningReady: true});
            }
            ++readinessCalls;
            return htmlResponse();
        },
        mutationFetchFn: async () => jsonResponse({
            ok: true, rebooting: true, ip: '192.168.20.72',
        }, 202),
        nowFn: () => now,
        sleepFn: async milliseconds => { now += milliseconds; },
        readinessDeadlineMs: 2000,
        readinessPollMs: 1000,
        onState: state => states.push(state),
    });

    assert.equal(await handoff.beginAttempt(), false);
    const callsBeforeRetry = readinessCalls;
    assert.equal(await handoff.retryReadiness(), false);
    assert.equal(readinessCalls, callsBeforeRetry + 1);
    assert.equal(states.at(-1).phase, 'readiness_timeout');
    assert.equal(states.at(-1).retry, true);
});

test('an unscheduled completion is retried only by an explicit action', async () => {
    let completionCalls = 0;
    const phases = [];
    const handoff = new ProvisioningHandoff({
        fetchFn: async url => url === '/api/wifi/status'
            ? jsonResponse({state: 'connected', ip: '192.168.20.72', provisioningReady: true})
            : jsonResponse({ok: true, mode: 'station'}),
        mutationFetchFn: async () => {
            ++completionCalls;
            return completionCalls === 1
                ? jsonResponse({ok: false, error: 'reboot_unavailable'}, 503)
                : jsonResponse({ok: true, rebooting: true, ip: '192.168.20.72'}, 202);
        },
        onState: state => phases.push(state.phase),
    });
    assert.equal(await handoff.beginAttempt(), false);
    assert.equal(completionCalls, 1);
    assert.equal(phases.at(-1), 'completion_unavailable');
    assert.equal(await handoff.retryCompletion(), true);
    assert.equal(completionCalls, 2);
    assert.equal(phases.at(-1), 'station_ready');
});
