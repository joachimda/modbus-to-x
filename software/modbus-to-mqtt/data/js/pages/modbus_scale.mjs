export function parseConfiguredScale(value) {
    return Number(value ?? 1);
}

export function validateDatapointScale(functionCode, scale) {
    if (!Number.isFinite(scale)) return 'finite';
    if ((functionCode === 6 || functionCode === 16) && scale === 0) return 'non_zero';
    return null;
}
