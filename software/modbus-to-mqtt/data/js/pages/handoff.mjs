const JSON_TYPE = 'application/json';

function mediaType(response) {
    return (response.headers.get('Content-Type') || '').split(';', 1)[0].trim().toLowerCase();
}

async function jsonBody(response) {
    if (mediaType(response) !== JSON_TYPE) throw new Error('unexpected_content_type');
    try {
        return await response.json();
    } catch (_) {
        throw new Error('malformed_json');
    }
}

function isIpv4Address(value) {
    if (typeof value !== 'string') return false;
    const parts = value.split('.');
    return parts.length === 4
        && parts.some(part => part !== '0')
        && parts.every(part => /^(0|[1-9]\d{0,2})$/.test(part) && Number(part) <= 255);
}

function isExactStationReady(body) {
    if (!body || typeof body !== 'object' || Array.isArray(body)) return false;
    const keys = Object.keys(body).sort();
    return keys.length === 2 && keys[0] === 'mode' && keys[1] === 'ok'
        && body.ok === true && body.mode === 'station';
}

export class ProvisioningHandoff {
    constructor({
        fetchFn = fetch,
        mutationFetchFn = fetch,
        sleepFn = ms => new Promise(resolve => setTimeout(resolve, ms)),
        nowFn = () => Date.now(),
        setTimeoutFn = setTimeout,
        clearTimeoutFn = clearTimeout,
        abortControllerFactory = () => new AbortController(),
        stationUrlsFn = ip => ({
            readyUrl: `http://${ip}/api/system/ready`,
            dashboardUrl: `http://${ip}/`,
        }),
        onState = () => {},
        statusUrl = '/api/wifi/status',
        completionUrl = '/api/wifi/ap_off',
        statusPollMs = 1000,
        readinessPollMs = 1000,
        readinessDeadlineMs = 90000,
        requestTimeoutMs = 3000,
    } = {}) {
        this.fetchFn = fetchFn;
        this.mutationFetchFn = mutationFetchFn;
        this.sleepFn = sleepFn;
        this.nowFn = nowFn;
        this.setTimeoutFn = setTimeoutFn;
        this.clearTimeoutFn = clearTimeoutFn;
        this.abortControllerFactory = abortControllerFactory;
        this.stationUrlsFn = stationUrlsFn;
        this.onState = onState;
        this.statusUrl = statusUrl;
        this.completionUrl = completionUrl;
        this.statusPollMs = statusPollMs;
        this.readinessPollMs = readinessPollMs;
        this.readinessDeadlineMs = readinessDeadlineMs;
        this.requestTimeoutMs = requestTimeoutMs;
        this.generation = 0;
        this.completionInFlight = false;
        this.completionRequested = false;
        this.readinessInFlight = false;
        this.stationIp = '';
        this.rebootKnownPending = false;
    }

    beginAttempt(timeoutMs = 35000) {
        const generation = ++this.generation;
        this.completionInFlight = false;
        this.completionRequested = false;
        this.readinessInFlight = false;
        this.stationIp = '';
        this.rebootKnownPending = false;
        this.emit('connecting');
        return this.pollStatus(generation, this.nowFn() + timeoutMs);
    }

    cancel(emitState = true) {
        ++this.generation;
        this.completionInFlight = false;
        this.readinessInFlight = false;
        if (emitState) this.emit('cancelled');
    }

    async retryCompletion() {
        if (!this.stationIp || this.completionInFlight || this.rebootKnownPending) return false;
        return this.requestCompletion(this.generation, this.stationIp, true);
    }

    async retryReadiness() {
        if (!this.stationIp || this.readinessInFlight || !this.rebootKnownPending) return false;
        return this.pollReadiness(this.generation, this.stationIp, true);
    }

    async pollStatus(generation, deadline) {
        while (generation === this.generation) {
            try {
                const response = await this.fetchFn(this.statusUrl, {cache: 'no-store'});
                if (generation !== this.generation) return false;
                if (response.status !== 200) throw new Error(`status_http_${response.status}`);
                const status = await jsonBody(response);
                if (generation !== this.generation) return false;
                const state = String(status?.state || '').toLowerCase();
                const ip = typeof status?.ip === 'string' ? status.ip : '';
                this.emit('status', {status, state, ip});

                if (state === 'connected') {
                    this.stationIp = ip;
                    if (status.provisioningReady === true && isIpv4Address(ip)) {
                        this.emit('ready_for_completion', {ip});
                        return this.requestCompletion(generation, ip, false);
                    }
                    this.emit('connected_not_ready', {ip, reason: status.reason || 'PERSISTENCE_REQUIRED'});
                    return false;
                }
                if (state === 'failed' || state === 'disconnected') {
                    this.emit('connection_failed', {reason: status.reason || 'UNKNOWN'});
                    return false;
                }
            } catch (error) {
                if (generation !== this.generation) return false;
                this.emit('status_warning', {error: error.message});
            }

            if (this.nowFn() >= deadline) {
                this.emit('connection_timeout');
                return false;
            }
            await this.sleepFn(this.statusPollMs);
        }
        return false;
    }

    async requestCompletion(generation, expectedIp, explicit) {
        if (generation !== this.generation || this.completionInFlight) return false;
        if (!explicit && this.completionRequested) return false;
        this.completionRequested = true;
        this.completionInFlight = true;
        this.emit('completion_requesting', {ip: expectedIp, explicit});
        try {
            const response = await this.mutationFetchFn(this.completionUrl, {method: 'POST'});
            if (generation !== this.generation) return false;
            const body = await jsonBody(response);
            if (generation !== this.generation) return false;

            if (response.status === 202 && body?.ok === true && body?.rebooting === true
                && body.ip === expectedIp && isIpv4Address(body.ip)) {
                this.stationIp = body.ip;
                this.rebootKnownPending = true;
                this.emit('reboot_accepted', {ip: body.ip});
                this.completionInFlight = false;
                return this.pollReadiness(generation, body.ip);
            }
            if (response.status === 409 && body?.ok === false && body?.error === 'reboot_pending') {
                this.rebootKnownPending = true;
                this.emit('reboot_pending', {ip: expectedIp});
                this.completionInFlight = false;
                return this.pollReadiness(generation, expectedIp);
            }
            if (response.status === 409 && body?.ok === false && body?.error === 'provisioning_not_ready') {
                this.emit('completion_rejected', {ip: expectedIp});
                return false;
            }
            if (response.status === 503 && body?.ok === false && body?.error === 'reboot_unavailable') {
                this.emit('completion_unavailable', {ip: expectedIp});
                return false;
            }
            this.emit('completion_invalid', {ip: expectedIp, status: response.status});
            return false;
        } catch (error) {
            if (generation === this.generation) this.emit('completion_invalid', {ip: expectedIp, error: error.message});
            return false;
        } finally {
            this.completionInFlight = false;
        }
    }

    async pollReadiness(generation, ip, singleAttempt = false) {
        if (generation !== this.generation || this.readinessInFlight || !isIpv4Address(ip)) return false;
        this.readinessInFlight = true;
        const deadline = this.nowFn() + this.readinessDeadlineMs;
        const {readyUrl, dashboardUrl} = this.stationUrlsFn(ip);
        this.emit('waiting_for_station', {ip, dashboardUrl, retry: singleAttempt});
        try {
            while (generation === this.generation && this.nowFn() < deadline) {
                try {
                    const response = await this.fetchWithTimeout(readyUrl);
                    if (generation !== this.generation) return false;
                    if (response.type === 'opaque' || response.status !== 200 || response.redirected) {
                        throw new Error('station_not_ready');
                    }
                    const body = await jsonBody(response);
                    if (!isExactStationReady(body)) throw new Error('invalid_ready_response');
                    this.emit('station_ready', {ip, dashboardUrl});
                    return true;
                } catch (error) {
                    if (generation !== this.generation) return false;
                    this.emit('readiness_wait', {ip, error: error.message, retry: singleAttempt});
                }
                if (singleAttempt) break;
                await this.sleepFn(this.readinessPollMs);
            }
            if (generation === this.generation) {
                this.emit('readiness_timeout', {ip, dashboardUrl, retry: singleAttempt});
            }
            return false;
        } finally {
            this.readinessInFlight = false;
        }
    }

    async fetchWithTimeout(url) {
        const controller = this.abortControllerFactory();
        const timer = this.setTimeoutFn(() => controller.abort(), this.requestTimeoutMs);
        try {
            return await this.fetchFn(url, {
                cache: 'no-store',
                mode: 'cors',
                redirect: 'error',
                signal: controller.signal,
            });
        } finally {
            this.clearTimeoutFn(timer);
        }
    }

    emit(phase, detail = {}) {
        this.onState({phase, ...detail});
    }
}

export {isExactStationReady, isIpv4Address};
