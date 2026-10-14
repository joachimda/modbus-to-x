export function parseConfiguredScale(value) {
    return Number(value ?? 1);
}

export function validateDatapointScale(functionCode, scale) {
    if (!Number.isFinite(scale)) return 'finite';
    // Register writes: FC6, FC16 and Wavin 0x44 (68).
    if ((functionCode === 6 || functionCode === 16 || functionCode === 68) && scale === 0) return 'non_zero';
    return null;
}
