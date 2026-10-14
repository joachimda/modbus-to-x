// Register widths and shape rules per Modbus data type. Mirrors the
// firmware's ModbusValueCodec::wordWidth and ModbusConfigDocument rules so
// the editor shows the same lengths and rejections the device applies.

const CANONICAL_TYPES = Object.freeze({
    text: "TEXT",
    int16: "int16",
    int32: "int32",
    int64: "int64",
    uint16: "uint16",
    uint32: "uint32",
    uint64: "uint64",
    float32: "float32",
});

// 16-bit registers per value; TEXT has no fixed width.
export const WORD_WIDTH = Object.freeze({
    TEXT: 0,
    int16: 1,
    uint16: 1,
    int32: 2,
    uint32: 2,
    float32: 2,
    int64: 4,
    uint64: 4,
});

export const SHAPE_MESSAGES = Object.freeze({
    fc6_multiword: "FC6 writes exactly one register; use FC16 for 32-bit and 64-bit data types",
    slice_multiword: "registerSlice requires a 16-bit data type",
    length_below_width: "numOfRegisters is smaller than the data type's register width",
    length_not_width_fc16: "FC16 numOfRegisters must equal the data type's register width",
    word_order_invalid: "wordOrder must be \"big\" or \"little\"",
});

export function normalizeDataType(type) {
    const key = String(type ?? "uint16").trim().toLowerCase();
    return CANONICAL_TYPES[key] ?? "uint16";
}

export function wordWidth(type) {
    return WORD_WIDTH[normalizeDataType(type)];
}

export function isMultiWordType(type) {
    return wordWidth(type) > 1;
}

export function normalizeWordOrder(value) {
    const raw = String(value ?? "big").trim().toLowerCase();
    return raw === "little" || raw === "little_endian" || raw === "1" ? "little" : "big";
}

// Returns the register count the firmware will use for a datapoint:
// FC5 always 1; FC6 1 for 16-bit types (multi-word types cannot be repaired
// and are left for validation); FC16 exactly the width; FC3/FC4 at least
// the width. TEXT and bit functions keep the requested length.
export function coerceRegisterCount(functionCode, type, length) {
    const fn = Number(functionCode);
    const requested = Number.isInteger(length) && length > 0 ? length : 1;
    const width = wordWidth(type);
    let next = requested;
    if (fn === 5) {
        next = 1;
    } else if (fn === 6) {
        next = width > 1 ? requested : 1;
    } else if (fn === 16) {
        next = width > 0 ? width : 1;
    } else if ((fn === 3 || fn === 4) && requested < width) {
        next = width;
    }
    return { length: next, changed: next !== requested };
}

// Validates a schema datapoint's shape; returns the codes of every problem
// (keys of SHAPE_MESSAGES), or an empty array when consistent.
export function validateDatapointShape(point) {
    const codes = [];
    const fn = Number(point?.function);
    const width = wordWidth(point?.dataType);
    const length = Number(point?.numOfRegisters);
    const slice = point?.registerSlice == null
        ? "full"
        : String(point.registerSlice).trim().toLowerCase();
    if (fn === 6 && width > 1) {
        codes.push("fc6_multiword");
    }
    if (slice !== "full" && width !== 1) {
        codes.push("slice_multiword");
    }
    if ((fn === 3 || fn === 4) && width > 0 && Number.isInteger(length) && length < width) {
        codes.push("length_below_width");
    }
    if (fn === 16 && width > 0 && Number.isInteger(length) && length !== width) {
        codes.push("length_not_width_fc16");
    }
    if (point?.wordOrder != null) {
        const order = String(point.wordOrder).trim().toLowerCase();
        if (order !== "big" && order !== "little") {
            codes.push("word_order_invalid");
        }
    }
    return codes;
}

// First register address after a datapoint: its width or configured length,
// whichever is larger (TEXT and coils use the length).
export function nextAddress(address, type, length) {
    const base = Number.isInteger(address) && address >= 0 ? address : 0;
    const count = Number.isInteger(length) && length > 0 ? length : 1;
    return base + Math.max(wordWidth(type), count);
}
