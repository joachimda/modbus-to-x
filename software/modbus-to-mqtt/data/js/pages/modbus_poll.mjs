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

const secondsFromMilliseconds = (milliseconds) => {
    if (typeof milliseconds !== "number" || !Number.isFinite(milliseconds)) return 0;
    const seconds = Math.round(milliseconds / 1000);
    return Math.min(MAX_LEGACY_POLL_INTERVAL_SECONDS, Math.max(0, seconds));
};

// The editor works in whole seconds. Like the firmware, it reads
// poll_interval_ms first when a datapoint carries both fields. The stored
// milliseconds value is kept beside the seconds shown, so an untouched
// datapoint is written back unchanged (see pollIntervalToSchema).
export function pollIntervalFromSchema(datapoint) {
    if (datapoint != null && hasOwn(datapoint, "poll_interval_ms")) {
        return {
            poll_secs: secondsFromMilliseconds(datapoint.poll_interval_ms),
            poll_ms: datapoint.poll_interval_ms,
        };
    }
    const seconds = Number(datapoint?.poll_interval);
    return {poll_secs: Number.isFinite(seconds) ? seconds : 0};
}

// Writes poll_interval_ms back as stored while the seconds shown still match
// it: a sub-second or out-of-range value is neither rounded away nor hidden
// from validation. Once the seconds are edited, poll_interval replaces it.
export function pollIntervalToSchema(datapoint) {
    const seconds = Number(datapoint?.poll_secs ?? 0);
    if (datapoint != null && hasOwn(datapoint, "poll_ms")
        && secondsFromMilliseconds(datapoint.poll_ms) === seconds) {
        return {poll_interval_ms: datapoint.poll_ms};
    }
    return seconds !== 0 ? {poll_interval: seconds} : {};
}
