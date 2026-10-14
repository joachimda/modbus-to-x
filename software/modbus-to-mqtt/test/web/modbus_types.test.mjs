import test from 'node:test';
import assert from 'node:assert/strict';

import {
    WORD_WIDTH,
    SHAPE_MESSAGES,
    WAVIN_MAX_REGISTERS,
    coerceRegisterCount,
    isMultiWordType,
    isWavinFunction,
    nextAddress,
    normalizeDataType,
    normalizeWordOrder,
    validateDatapointShape,
    wordWidth,
} from '../../data/js/pages/modbus_types.mjs';

test('data types map to their register widths', () => {
    assert.deepEqual(WORD_WIDTH, {
        TEXT: 0, int16: 1, uint16: 1, int32: 2, uint32: 2, float32: 2, int64: 4, uint64: 4,
    });
    assert.equal(wordWidth('int32'), 2);
    assert.equal(wordWidth('FLOAT32'), 2);
    assert.equal(wordWidth('uint64'), 4);
    assert.equal(wordWidth('TEXT'), 0);
    assert.equal(wordWidth('text'), 0);
    assert.equal(wordWidth(undefined), 1);
    assert.equal(wordWidth('bogus'), 1);
    assert.equal(normalizeDataType(' Int16 '), 'int16');
    assert.equal(normalizeDataType('bogus'), 'uint16');
    assert.equal(isMultiWordType('uint16'), false);
    assert.equal(isMultiWordType('int64'), true);
    assert.equal(isMultiWordType('TEXT'), false);
});

test('word order normalises to big or little with big as the default', () => {
    assert.equal(normalizeWordOrder('LITTLE'), 'little');
    assert.equal(normalizeWordOrder(' little '), 'little');
    assert.equal(normalizeWordOrder(1), 'little');
    assert.equal(normalizeWordOrder('big'), 'big');
    assert.equal(normalizeWordOrder(undefined), 'big');
    assert.equal(normalizeWordOrder(null), 'big');
    assert.equal(normalizeWordOrder('sideways'), 'big');
});

test('register counts are coerced like the firmware does', () => {
    assert.deepEqual(coerceRegisterCount(3, 'int32', 1), { length: 2, changed: true });
    assert.deepEqual(coerceRegisterCount(4, 'int64', 2), { length: 4, changed: true });
    assert.deepEqual(coerceRegisterCount(3, 'uint16', 3), { length: 3, changed: false });
    assert.deepEqual(coerceRegisterCount(16, 'int16', 3), { length: 1, changed: true });
    assert.deepEqual(coerceRegisterCount(16, 'float32', 1), { length: 2, changed: true });
    assert.deepEqual(coerceRegisterCount(16, 'TEXT', 3), { length: 1, changed: true });
    assert.deepEqual(coerceRegisterCount(6, 'uint16', 2), { length: 1, changed: true });
    assert.deepEqual(coerceRegisterCount(6, 'int32', 2), { length: 2, changed: false });
    assert.deepEqual(coerceRegisterCount(5, 'uint16', 8), { length: 1, changed: true });
    assert.deepEqual(coerceRegisterCount(1, 'int32', 1), { length: 1, changed: false });
    assert.deepEqual(coerceRegisterCount(2, 'uint16', 16), { length: 16, changed: false });
    assert.deepEqual(coerceRegisterCount(3, 'TEXT', 10), { length: 10, changed: false });
    assert.deepEqual(coerceRegisterCount(3, 'uint16', 0), { length: 1, changed: false });
    assert.deepEqual(coerceRegisterCount(3, 'uint16', Number.NaN), { length: 1, changed: false });
});

test('datapoint shapes report every inconsistency', () => {
    assert.deepEqual(validateDatapointShape({ function: 6, dataType: 'int32', numOfRegisters: 2 }),
        ['fc6_multiword']);
    assert.deepEqual(validateDatapointShape({ function: 6, dataType: 'uint16', numOfRegisters: 1 }), []);
    assert.deepEqual(validateDatapointShape({ function: 3, dataType: 'uint32', numOfRegisters: 2, registerSlice: 'low_byte' }),
        ['slice_multiword']);
    assert.deepEqual(validateDatapointShape({ function: 3, dataType: 'TEXT', numOfRegisters: 4, registerSlice: 'high_byte' }),
        ['slice_multiword']);
    assert.deepEqual(validateDatapointShape({ function: 3, dataType: 'int16', numOfRegisters: 1, registerSlice: 'low_byte' }), []);
    assert.deepEqual(validateDatapointShape({ function: 16, dataType: 'int32', numOfRegisters: 1 }),
        ['length_not_width_fc16']);
    assert.deepEqual(validateDatapointShape({ function: 3, dataType: 'int64', numOfRegisters: 2 }),
        ['length_below_width']);
    assert.deepEqual(validateDatapointShape({ function: 4, dataType: 'int64', numOfRegisters: 6 }), []);
    assert.deepEqual(validateDatapointShape({ function: 3, dataType: 'float32', numOfRegisters: 2, wordOrder: 'little' }), []);
    assert.deepEqual(validateDatapointShape({ function: 3, dataType: 'float32', numOfRegisters: 2, wordOrder: 'middle' }),
        ['word_order_invalid']);
    assert.deepEqual(validateDatapointShape({ function: 6, dataType: 'float32', numOfRegisters: 1, registerSlice: 'low_byte' }),
        ['fc6_multiword', 'slice_multiword']);
    assert.deepEqual(validateDatapointShape({ function: 1, dataType: 'int32', numOfRegisters: 1 }), []);
    for (const code of ['fc6_multiword', 'slice_multiword', 'length_below_width', 'length_not_width_fc16', 'word_order_invalid']) {
        assert.equal(typeof SHAPE_MESSAGES[code], 'string');
    }
});

test('the next address skips the registers a datapoint occupies', () => {
    assert.equal(nextAddress(3, 100, 'int32', 1), 102);
    assert.equal(nextAddress(4, 100, 'uint16', 1), 101);
    assert.equal(nextAddress(3, 100, 'uint16', 3), 103);
    assert.equal(nextAddress(16, 100, 'int64', 2), 104);
    assert.equal(nextAddress(6, 100, 'int16', 1), 101);
    assert.equal(nextAddress(3, 100, 'TEXT', 5), 105);
    assert.equal(nextAddress(3, 0, 'TEXT', 0), 1);
    assert.equal(nextAddress(3, -5, 'uint16', 1), 1);
    assert.equal(nextAddress(3, undefined, 'float32', undefined), 2);
});

test('the next address of a coil or discrete datapoint ignores the data type', () => {
    assert.equal(nextAddress(1, 10, 'int32', 1), 11);
    assert.equal(nextAddress(2, 10, 'int64', 8), 18);
    assert.equal(nextAddress(5, 10, 'uint32', 1), 11);
    assert.equal(nextAddress('5', 10, 'int64', 1), 11);
    assert.equal(nextAddress(1, 10, 'float32', undefined), 11);
});

test('Wavin functions follow the FC3/FC6 register rules', () => {
    assert.equal(isWavinFunction(67), true);
    assert.equal(isWavinFunction('68'), true);
    assert.equal(isWavinFunction(3), false);
    assert.deepEqual(coerceRegisterCount(67, 'int32', 1), { length: 2, changed: true });
    assert.deepEqual(coerceRegisterCount(68, 'int16', 3), { length: 1, changed: true });
    assert.deepEqual(validateDatapointShape({ function: 68, dataType: 'int32', numOfRegisters: 2, address: 0 }),
        ['fc6_multiword']);
    assert.deepEqual(validateDatapointShape({ function: 67, dataType: 'int64', numOfRegisters: 2, address: 4 }),
        ['length_below_width']);
});

test('Wavin datapoints need a one-byte index, category and page', () => {
    const point = { function: 67, dataType: 'int16', numOfRegisters: 1, address: 4, category: 1, page: 3 };
    assert.deepEqual(validateDatapointShape(point), []);
    assert.deepEqual(validateDatapointShape({ ...point, category: undefined, page: undefined }), []);
    assert.deepEqual(validateDatapointShape({ ...point, address: 256 }), ['wavin_location_invalid']);
    assert.deepEqual(validateDatapointShape({ ...point, category: 300 }), ['wavin_location_invalid']);
    assert.deepEqual(validateDatapointShape({ ...point, page: -1 }), ['wavin_location_invalid']);
    assert.deepEqual(validateDatapointShape({ ...point, numOfRegisters: WAVIN_MAX_REGISTERS + 1 }),
        ['wavin_length_invalid']);
    // Standard functions keep their 16-bit addresses and ignore the fields.
    assert.deepEqual(validateDatapointShape({ function: 3, dataType: 'int16', numOfRegisters: 1, address: 4000, category: 999 }), []);
    for (const code of ['wavin_location_invalid', 'wavin_length_invalid']) {
        assert.equal(typeof SHAPE_MESSAGES[code], 'string');
    }
});
