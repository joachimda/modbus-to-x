import test from 'node:test';
import assert from 'node:assert/strict';

import {
    channelForSelection,
    connectionFieldsForSelection,
    filterScanResults,
    isSameAccessPoint,
    normalizeBssid,
    reconcileScanRefresh,
    selectScanResult,
    shouldInvalidateScanSelection,
} from '../../data/js/pages/wifi_selection.mjs';

function network(ssid, bssid, channel, options = {}) {
    return {
        ssid,
        bssid,
        channel,
        secure: options.secure ?? true,
        rssi: options.rssi ?? -60,
        auth: options.auth ?? 'WPA2',
    };
}

test('filtered rows select the exact rendered result instead of the raw array position', () => {
    const leading = network('Filtered lead', '02:00:00:00:00:01', 1);
    const meshA = network('Mesh network', '02:00:00:00:00:0A', 6);
    const meshB = network('Mesh network', '02:00:00:00:00:0B', 11);
    const networks = [leading, meshA, meshB];

    const visibleNetworks = filterScanResults(networks, 'mesh');
    assert.deepEqual(visibleNetworks, [meshA, meshB]);

    const first = selectScanResult(null, visibleNetworks[0]).selection;
    const second = selectScanResult(first, visibleNetworks[1]).selection;
    assert.equal(first, meshA);
    assert.equal(channelForSelection(first), 6);
    assert.equal(second, meshB);
    assert.equal(channelForSelection(second), 11);
});

test('duplicate SSIDs remain independent and changing BSSID clears the password', () => {
    const meshA = network('Mesh network', '02:00:00:00:00:0A', 6);
    const meshB = network('Mesh network', '02:00:00:00:00:0B', 11);

    const initial = selectScanResult(null, meshA);
    assert.equal(initial.selection, meshA);
    assert.equal(initial.clearPassword, true);

    const changed = selectScanResult(meshA, meshB);
    assert.equal(changed.selection, meshB);
    assert.equal(changed.clearPassword, true);

    const sameBssid = selectScanResult(meshB, {
        ...meshB,
        bssid: meshB.bssid.toLowerCase(),
        channel: 1,
    });
    assert.equal(sameBssid.clearPassword, false);
});

test('filtering can hide and reveal a selection without changing its identity', () => {
    const selected = network('Workshop', '02:00:00:00:00:02', 11);
    const networks = [
        network('TestNet', '02:00:00:00:00:01', 6),
        selected,
    ];

    assert.equal(filterScanResults(networks, 'test').some(item => isSameAccessPoint(selected, item)), false);
    assert.equal(filterScanResults(networks, '').some(item => isSameAccessPoint(selected, item)), true);
    assert.equal(filterScanResults(networks, 'WORK').some(item => isSameAccessPoint(selected, item)), true);
});

test('refresh rebinds only the same normalized BSSID and uses updated scan fields', () => {
    const selected = network('Mesh network', '02:00:00:00:00:0A', 6);
    const reorderedMatch = network('Mesh network', '02:00:00:00:00:0a', 13, {
        secure: false,
        auth: 'OPEN',
    });
    const nextNetworks = [
        network('Mesh network', '02:00:00:00:00:0B', 11),
        reorderedMatch,
    ];

    const result = reconcileScanRefresh(selected, nextNetworks);
    assert.equal(result.outcome, 'rebound');
    assert.equal(result.selection, reorderedMatch);
    assert.equal(channelForSelection(result.selection), 13);
    assert.equal(result.clearBssid, false);
    assert.equal(result.clearPassword, true);
    assert.equal(normalizeBssid(result.selection.bssid), normalizeBssid(selected.bssid));
});

test('secure same-BSSID refresh preserves the password', () => {
    const selected = network('Workshop', '02:00:00:00:00:02', 11);
    const refreshed = network('Workshop', '02:00:00:00:00:02', 1, {secure: true});

    const result = reconcileScanRefresh(selected, [refreshed]);
    assert.equal(result.outcome, 'rebound');
    assert.equal(result.selection, refreshed);
    assert.equal(result.clearPassword, false);
});

test('missing and BSSID-less selections degrade to manual values on successful refresh', () => {
    const missing = network('Workshop', '02:00:00:00:00:02', 11);
    const missingResult = reconcileScanRefresh(missing, [
        network('Workshop', '02:00:00:00:00:03', 11),
    ]);
    assert.equal(missingResult.outcome, 'manual');
    assert.equal(missingResult.selection, null);
    assert.equal(missingResult.clearBssid, true);
    assert.equal(missingResult.clearPassword, false);
    assert.equal(channelForSelection(missingResult.selection), 0);

    const bssidless = network('Hidden BSSID', '', 3);
    assert.equal(isSameAccessPoint(bssidless, bssidless), true);
    const bssidlessResult = reconcileScanRefresh(bssidless, [network('Hidden BSSID', '', 3)]);
    assert.equal(bssidlessResult.outcome, 'manual');
    assert.equal(bssidlessResult.selection, null);
    assert.equal(bssidlessResult.clearBssid, true);
});

test('failed refresh leaves the exact selection and form actions unchanged', () => {
    const selected = network('Workshop', '02:00:00:00:00:02', 11);
    const result = reconcileScanRefresh(selected, [], false);

    assert.equal(result.outcome, 'unchanged');
    assert.equal(result.selection, selected);
    assert.equal(result.clearBssid, false);
    assert.equal(result.clearPassword, false);
    assert.equal(channelForSelection(result.selection), 11);
});

test('manual SSID or BSSID edits invalidate the scan-derived channel', () => {
    const selected = network('Workshop', '02:00:00:00:00:02', 11);

    assert.equal(shouldInvalidateScanSelection(
        selected, 'Workshop', '02:00:00:00:00:02'), false);
    assert.equal(shouldInvalidateScanSelection(
        selected, 'Workshop', '02:00:00:00:00:02'.toLowerCase()), false);
    assert.equal(shouldInvalidateScanSelection(
        selected, 'Workshop manual', '02:00:00:00:00:02'), true);
    assert.equal(shouldInvalidateScanSelection(
        selected, 'Workshop', '02:00:00:00:00:99'), true);
    assert.equal(channelForSelection(null), 0);

    assert.deepEqual(connectionFieldsForSelection(null, ' 02:00:00:00:00:99 '), {
        bssid: '02:00:00:00:00:99',
        channel: 0,
    });
    assert.deepEqual(connectionFieldsForSelection(selected, selected.bssid), {
        bssid: selected.bssid,
        channel: 11,
    });
});
