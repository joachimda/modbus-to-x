import test from 'node:test';
import assert from 'node:assert/strict';

import {
    parseConfiguredScale,
    validateDatapointScale,
} from '../../data/js/pages/modbus_scale.mjs';

test('scale parsing preserves zero and negative values instead of defaulting them', () => {
    assert.equal(parseConfiguredScale(undefined), 1);
    assert.equal(parseConfiguredScale(null), 1);
    assert.equal(parseConfiguredScale(0), 0);
    assert.equal(parseConfiguredScale('-0.25'), -0.25);
    assert.equal(parseConfiguredScale('not-a-number'), Number.NaN);
});

test('only writable holding registers require a finite non-zero scale', () => {
    assert.equal(validateDatapointScale(6, 0), 'non_zero');
    assert.equal(validateDatapointScale(16, -0), 'non_zero');
    assert.equal(validateDatapointScale(6, Number.POSITIVE_INFINITY), 'finite');
    assert.equal(validateDatapointScale(16, Number.NaN), 'finite');
    assert.equal(validateDatapointScale(6, -0.25), null);
    assert.equal(validateDatapointScale(16, 0.5), null);
    assert.equal(validateDatapointScale(5, 0), null);
    assert.equal(validateDatapointScale(3, 0), null);
});
