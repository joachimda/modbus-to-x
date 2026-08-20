import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {once} from 'node:events';
import {fileURLToPath} from 'node:url';

import {ProvisioningHandoff} from '../../data/js/pages/handoff.mjs';

const projectRoot = fileURLToPath(new URL('../../', import.meta.url));
const serverScript = fileURLToPath(new URL('../../scripts/run_test_webserver.py', import.meta.url));
const baseUrl = 'http://127.0.0.1:18080';

function sleep(milliseconds) {
    return new Promise(resolve => setTimeout(resolve, milliseconds));
}

async function waitForServer(process) {
    for (let attempt = 0; attempt < 50; ++attempt) {
        if (process.exitCode !== null) throw new Error(`mock_server_exited_${process.exitCode}`);
        try {
            const response = await fetch(`${baseUrl}/api/wifi/status`);
            if (response.status === 200) return;
        } catch (_) {
            // The process may not have bound its socket yet.
        }
        await sleep(50);
    }
    throw new Error('mock_server_start_timeout');
}

async function stopServer(process) {
    if (process.exitCode !== null) return;
    process.kill('SIGTERM');
    await Promise.race([once(process, 'exit'), sleep(2000)]);
    if (process.exitCode === null) process.kill('SIGKILL');
}

async function runScenario(scenario) {
    const server = spawn('python3', [
        serverScript,
        '--host', '127.0.0.1',
        '--port', '18080',
        '--no-open',
        '--provisioning-scenario', scenario,
    ], {cwd: projectRoot, stdio: 'ignore'});

    try {
        await waitForServer(server);
        const connectResponse = await fetch(`${baseUrl}/api/wifi/connect`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'X-MBX-Request': '1',
            },
            body: JSON.stringify({ssid: 'TestNet', password: 'test-password', save: true}),
        });
        assert.equal(connectResponse.status, 202);

        const phases = [];
        const fetchWithCorsSemantics = async (url, options) => {
            const response = await fetch(url, options);
            if (scenario === 'readiness-no-cors' && String(url).endsWith('/api/system/ready')
                && response.headers.get('Access-Control-Allow-Origin') !== '*') {
                return {
                    status: 0,
                    type: 'opaque',
                    redirected: false,
                    headers: new Headers(),
                    json: async () => ({}),
                };
            }
            return response;
        };
        const handoff = new ProvisioningHandoff({
            fetchFn: fetchWithCorsSemantics,
            mutationFetchFn: (url, options = {}) => fetch(url, {
                ...options,
                headers: {...options.headers, 'X-MBX-Request': '1'},
            }),
            statusUrl: `${baseUrl}/api/wifi/status`,
            completionUrl: `${baseUrl}/api/wifi/ap_off`,
            stationUrlsFn: () => ({
                readyUrl: `${baseUrl}/api/system/ready`,
                dashboardUrl: `${baseUrl}/`,
            }),
            statusPollMs: 100,
            readinessPollMs: 100,
            readinessDeadlineMs: 3000,
            requestTimeoutMs: 1000,
            onState: state => phases.push(state.phase),
        });

        return {result: await handoff.beginAttempt(6000), phases};
    } finally {
        await stopServer(server);
    }
}

test('handoff module honors every stateful mock-server provisioning scenario', async () => {
    const cases = [
        ['success', true, 'station_ready'],
        ['delayed', true, 'station_ready'],
        ['temporary', false, 'connected_not_ready'],
        ['completion-rejected', false, 'completion_rejected'],
        ['completion-unavailable', false, 'completion_unavailable'],
        ['completion-html', false, 'completion_invalid'],
        ['completion-malformed', false, 'completion_invalid'],
        ['readiness-html', false, 'readiness_timeout'],
        ['readiness-malformed', false, 'readiness_timeout'],
        ['readiness-unrelated', false, 'readiness_timeout'],
        ['readiness-redirect', false, 'readiness_timeout'],
        ['readiness-no-cors', false, 'readiness_timeout'],
        ['readiness-timeout', false, 'readiness_timeout'],
    ];

    for (const [scenario, expectedResult, expectedFinalPhase] of cases) {
        const {result, phases} = await runScenario(scenario);
        assert.equal(result, expectedResult, scenario);
        assert.equal(phases.at(-1), expectedFinalPhase, scenario);
    }
});
