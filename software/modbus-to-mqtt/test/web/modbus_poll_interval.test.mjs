import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';

import {
    MAX_LEGACY_POLL_INTERVAL_SECONDS,
    MAX_POLL_INTERVAL_MS,
    pollIntervalValidationError,
} from '../../data/js/pages/modbus_poll.mjs';

test('browser validation enforces rollover-safe polling interval bounds', () => {
    for (const value of [0, MAX_POLL_INTERVAL_MS]) {
        assert.equal(pollIntervalValidationError({poll_interval_ms: value}), null);
    }
    for (const value of [MAX_POLL_INTERVAL_MS + 1, -1, 1.5, '1000', Number.NaN]) {
        assert.match(pollIntervalValidationError({poll_interval_ms: value}), /poll_interval_ms/);
    }

    for (const value of [0, MAX_LEGACY_POLL_INTERVAL_SECONDS]) {
        assert.equal(pollIntervalValidationError({poll_interval: value}), null);
    }
    for (const value of [MAX_LEGACY_POLL_INTERVAL_SECONDS + 1, -1, 1.5, '1000', Number.NaN]) {
        assert.match(pollIntervalValidationError({poll_interval: value}), /poll_interval/);
    }
});

test('configuration schema declares the same polling interval maxima', async () => {
    const schemaUrl = new URL('../../data/conf/schema.json', import.meta.url);
    const schema = JSON.parse(await readFile(schemaUrl, 'utf8'));
    const datapointProperties = schema.properties.devices.items.properties
        .dataPoints.items.properties;

    assert.equal(datapointProperties.poll_interval.minimum, 0);
    assert.equal(datapointProperties.poll_interval.maximum,
        MAX_LEGACY_POLL_INTERVAL_SECONDS);
    assert.equal(datapointProperties.poll_interval_ms.minimum, 0);
    assert.equal(datapointProperties.poll_interval_ms.maximum, MAX_POLL_INTERVAL_MS);
});
