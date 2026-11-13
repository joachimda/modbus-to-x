export const MAX_POLL_INTERVAL_MS = 2147483647;
export const MAX_LEGACY_POLL_INTERVAL_SECONDS = 2147483;

const hasOwn = (object, property) => Object.prototype.hasOwnProperty.call(object, property);

export function pollIntervalValidationError(datapoint) {
    if (hasOwn(datapoint, "poll_interval_ms")) {
        if (!Number.isInteger(datapoint.poll_interval_ms)
            || datapoint.poll_interval_ms < 0
            || datapoint.poll_interval_ms > MAX_POLL_INTERVAL_MS) {
            return `poll_interval_ms must be a whole number from 0 to ${MAX_POLL_INTERVAL_MS} milliseconds`;
        }
    }

    if (hasOwn(datapoint, "poll_interval")) {
        if (!Number.isInteger(datapoint.poll_interval)
            || datapoint.poll_interval < 0
            || datapoint.poll_interval > MAX_LEGACY_POLL_INTERVAL_SECONDS) {
            return `poll_interval must be a whole number from 0 to ${MAX_LEGACY_POLL_INTERVAL_SECONDS} seconds`;
        }
    }

    return null;
}
