import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';

import {
    MAX_LEGACY_POLL_INTERVAL_SECONDS,
    MAX_POLL_INTERVAL_MS,
    pollIntervalFromSchema,
    pollIntervalToSchema,
    pollIntervalValidationError,
} from '../../data/js/pages/modbus_poll.mjs';

test('browser validation enforces rollover-safe polling interval bounds', () => {
    for (const value of [0, MAX_POLL_INTERVAL_MS]) {
        assert.equal(pollIntervalValidationError({poll_interval_ms: value}), null);
    }
    for (const value of [MAX_POLL_INTERVAL_MS + 1, -1, 1.5, '1000', Number.NaN]) {
        assert.match(pollIntervalValidationError({poll_interval_ms: value}), /^poll_interval_ms must/);
    }

    for (const value of [0, MAX_LEGACY_POLL_INTERVAL_SECONDS]) {
        assert.equal(pollIntervalValidationError({poll_interval: value}), null);
    }
    for (const value of [MAX_LEGACY_POLL_INTERVAL_SECONDS + 1, -1, 1.5, '1000', Number.NaN]) {
        assert.match(pollIntervalValidationError({poll_interval: value}), /^poll_interval must/);
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

test('an invalid value in either interval field is reported when both are present', () => {
    assert.equal(pollIntervalValidationError({poll_interval_ms: 500, poll_interval: 30}), null);
    assert.match(pollIntervalValidationError({poll_interval_ms: -1, poll_interval: 30}),
        /^poll_interval_ms must/);
    assert.match(pollIntervalValidationError({poll_interval_ms: 500, poll_interval: 1.5}),
        /^poll_interval must/);
});

test('the editor reads poll_interval_ms first, as the firmware does', () => {
    assert.deepEqual(pollIntervalFromSchema({poll_interval_ms: 5000, poll_interval: 30}),
        {poll_secs: 5, poll_ms: 5000});
    assert.deepEqual(pollIntervalFromSchema({poll_interval: 30}), {poll_secs: 30});
    assert.deepEqual(pollIntervalFromSchema({}), {poll_secs: 0});
    assert.deepEqual(pollIntervalFromSchema({poll_interval: 'soon'}), {poll_secs: 0});
    // The largest valid milliseconds value rounds up past the seconds maximum.
    assert.equal(pollIntervalFromSchema({poll_interval_ms: MAX_POLL_INTERVAL_MS}).poll_secs,
        MAX_LEGACY_POLL_INTERVAL_SECONDS);
});

test('an untouched poll_interval_ms is written back as stored', () => {
    for (const stored of [400, 500, 1500, 5000, MAX_POLL_INTERVAL_MS]) {
        const ui = pollIntervalFromSchema({poll_interval_ms: stored, poll_interval: 30});
        assert.deepEqual(pollIntervalToSchema(ui), {poll_interval_ms: stored});
    }
});

test('an invalid stored poll_interval_ms still reaches the save-time check', () => {
    for (const stored of [3000000000, -5, 1.5, '1000', null]) {
        const saved = pollIntervalToSchema(pollIntervalFromSchema({poll_interval_ms: stored}));
        assert.deepEqual(saved, {poll_interval_ms: stored});
        assert.match(pollIntervalValidationError(saved), /^poll_interval_ms must/);
    }
});

test('an edited interval is saved in seconds', () => {
    const ui = pollIntervalFromSchema({poll_interval_ms: 1500});
    assert.deepEqual(pollIntervalToSchema({...ui, poll_secs: 10}), {poll_interval: 10});
    // The page drops poll_ms when the field is edited, even to the value shown.
    assert.deepEqual(pollIntervalToSchema({poll_secs: 2}), {poll_interval: 2});
    assert.deepEqual(pollIntervalToSchema({poll_secs: 0}), {});
    assert.deepEqual(pollIntervalToSchema({}), {});
    // Out-of-range seconds are written as typed so that validation reports them.
    assert.deepEqual(pollIntervalToSchema({poll_secs: -1}), {poll_interval: -1});
});
