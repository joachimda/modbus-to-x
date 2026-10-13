const ACTIVE_STATES = new Set([
    "quiescing",
    "awaiting_upload",
    "downloading_writing_firmware",
    "verifying_firmware",
    "downloading_writing_filesystem",
    "verifying_filesystem",
    "failure_detected",
    "resuming",
    "rebooting",
]);

export function encodeOtaPassword(value) {
    const bytes = new TextEncoder().encode(value);
    let binary = "";
    for (const byte of bytes) binary += String.fromCharCode(byte);
    return btoa(binary);
}

export function formatOtaSnapshot(snapshot, prettyBytes = value => `${value} B`) {
    const labels = {
        idle: "Ready for an update",
        quiescing: "Preparing device services",
        awaiting_upload: "Ready for image upload",
        downloading_writing_firmware: "Downloading and writing firmware",
        verifying_firmware: "Verifying firmware",
        downloading_writing_filesystem: "Downloading and writing filesystem",
        verifying_filesystem: "Verifying filesystem",
        failure_detected: "Update failure detected",
        resuming: "Restoring device services",
        failed: "Update failed",
        rebooting: "Update verified; rebooting",
    };
    const state = String(snapshot?.state || snapshot?.stage || "idle");
    let message = labels[state] || "Update status";
    if (snapshot?.targetVersion) message += ` to ${snapshot.targetVersion}`;
    if (snapshot?.component && snapshot.component !== "none") {
        const received = Number(snapshot.receivedBytes);
        const total = Number(snapshot.totalBytes);
        if (Number.isFinite(received) && Number.isFinite(total) && total > 0) {
            message += ` — ${prettyBytes(received)} / ${prettyBytes(total)}`;
        } else if (Number.isFinite(received) && received > 0) {
            message += ` — ${prettyBytes(received)}`;
        }
        if (snapshot.componentPercentKnown) message += ` (${Number(snapshot.componentPercent) || 0}%)`;
    }
    if (snapshot?.overallPercentKnown) message += ` — overall ${Number(snapshot.overallPercent) || 0}%`;
    if (state === "failed" && snapshot?.error) message += `: ${snapshot.error}`;
    return message;
}

export class OtaController {
    constructor({
        api,
        fetchFn = globalThis.fetch?.bind(globalThis),
        EventSourceCtor = globalThis.EventSource,
        promptPassword = () => globalThis.prompt?.("OTA password:"),
        setTimeoutFn = globalThis.setTimeout?.bind(globalThis),
        clearTimeoutFn = globalThis.clearTimeout?.bind(globalThis),
        onSnapshot = () => {},
        onTransport = () => {},
        closeNormalStream = () => {},
        resumeNormalStream = () => {},
        onReboot = () => {},
        pollIntervalMs = 3000,
        retryMaximumMs = 15000,
    }) {
        this.api = api;
        this.fetchFn = fetchFn;
        this.EventSourceCtor = EventSourceCtor;
        this.promptPassword = promptPassword;
        this.setTimeoutFn = setTimeoutFn;
        this.clearTimeoutFn = clearTimeoutFn;
        this.onSnapshot = onSnapshot;
        this.onTransport = onTransport;
        this.closeNormalStream = closeNormalStream;
        this.resumeNormalStream = resumeNormalStream;
        this.onReboot = onReboot;
        this.pollIntervalMs = pollIntervalMs;
        this.retryMaximumMs = retryMaximumMs;
        this.sessionToken = null;
        this.operationId = 0;
        this.sequence = 0;
        this.source = null;
        this.pollTimer = null;
        this.retryTimer = null;
        this.pollInFlight = false;
        this.retryDelayMs = pollIntervalMs;
        this.awaitingUpload = null;
    }

    mutationHeaders(headers) {
        const result = new Headers(headers || {});
        result.set("X-MBX-Request", "1");
        return result;
    }

    async responseJson(response) {
        if (response.status === 204) return {};
        let body = null;
        try {
            body = await response.json();
        } catch (_) {
            // Preserve the status text when an error response has no JSON body.
        }
        if (!response.ok) {
            const error = new Error(body?.error || `${response.status} ${response.statusText}`);
            error.status = response.status;
            error.body = body;
            throw error;
        }
        if (body == null) throw new Error("invalid_json_response");
        return body;
    }

    async createSession() {
        const request = async authorization => {
            const headers = this.mutationHeaders();
            if (authorization) headers.set("Authorization", authorization);
            return this.fetchFn(this.api.OTA_SESSION, {
                method: "POST",
                cache: "no-store",
                headers,
            });
        };

        let response = await request(null);
        if (response.status === 401) {
            const password = this.promptPassword();
            if (password == null || password === "") throw new Error("auth_required");
            const authorization = `Bearer ${encodeOtaPassword(password)}`;
            response = await request(authorization);
        }
        const body = await this.responseJson(response);
        if (typeof body.token !== "string" || !/^[0-9a-fA-F]{64}$/.test(body.token)) {
            throw new Error("invalid_ota_session");
        }
        this.sessionToken = body.token;
        return body.token;
    }

    async ensureSession() {
        return this.sessionToken || this.createSession();
    }

    invalidateSession() {
        this.sessionToken = null;
    }

    async requestJson(url, init = {}, retry = true) {
        const token = await this.ensureSession();
        const method = String(init.method || "GET").toUpperCase();
        const headers = new Headers(init.headers || {});
        headers.set("X-OTA-Session", token);
        if (method !== "GET" && method !== "HEAD") headers.set("X-MBX-Request", "1");
        const response = await this.fetchFn(url, {
            cache: "no-store",
            ...init,
            headers,
        });
        if (response.status === 401 && retry) {
            this.invalidateSession();
            return this.requestJson(url, init, false);
        }
        const body = await this.responseJson(response);
        if (url === this.api.OTA_PASSWORD || url === this.api.FACTORY_RESET) this.invalidateSession();
        return body;
    }

    async apply() {
        this.closeNormalStream();
        try {
            const response = await this.requestJson(this.api.OTA_HTTP_APPLY, {method: "POST"});
            if (!response.started || !Number.isInteger(response.operationId) || response.operationId <= 0) {
                throw new Error(response.error || "apply_not_started");
            }
            this.monitor(response.operationId);
            return response;
        } catch (error) {
            const recovered = await this.recoverAfterInitiationFailure();
            if (!recovered) this.resumeNormalStream();
            if (recovered) return {ok: true, started: true, operationId: this.operationId, recovered: true};
            throw error;
        }
    }

    async manualUpload(component, file) {
        if (component !== "firmware" && component !== "filesystem") throw new Error("invalid_component");
        if (!file || !Number.isFinite(file.size) || file.size <= 0) throw new Error("invalid_file");
        this.closeNormalStream();
        let preflight;
        try {
            preflight = await this.requestJson(this.api.OTA_MANUAL_START, {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({component, size: file.size}),
            });
        } catch (error) {
            const recovered = await this.recoverAfterInitiationFailure();
            if (!recovered) {
                this.resumeNormalStream();
                throw error;
            }
            preflight = {operationId: this.operationId, recovered: true};
        }
        if (!Number.isInteger(preflight.operationId) || preflight.operationId <= 0) {
            this.resumeNormalStream();
            throw new Error("manual_preflight_invalid");
        }
        const awaitingUpload = this.waitForAwaitingUpload();
        this.monitor(preflight.operationId);
        await awaitingUpload;

        const form = new FormData();
        form.append(component, file, file.name);
        const token = await this.ensureSession();
        const headers = this.mutationHeaders({
            "X-OTA-Session": token,
            "X-OTA-Operation": String(preflight.operationId),
        });
        const uploadUrl = component === "firmware" ? this.api.OTA_FIRMWARE : this.api.OTA_FILESYSTEM;
        try {
            const response = await this.fetchFn(uploadUrl, {method: "POST", body: form, headers});
            return await this.responseJson(response);
        } catch (error) {
            if (await this.recoverAfterInitiationFailure()) {
                return {ok: true, recovered: true, operationId: preflight.operationId};
            }
            throw error;
        }
    }

    monitor(operationId) {
        this.stopMonitoring(false);
        this.operationId = operationId;
        this.sequence = 0;
        this.retryDelayMs = this.pollIntervalMs;
        this.openEventSource();
        this.pollNow();
    }

    stopMonitoring(resetOperation = true) {
        if (this.source) this.source.close();
        this.source = null;
        if (this.pollTimer != null) this.clearTimeoutFn(this.pollTimer);
        if (this.retryTimer != null) this.clearTimeoutFn(this.retryTimer);
        this.pollTimer = null;
        this.retryTimer = null;
        this.pollInFlight = false;
        if (resetOperation) {
            this.operationId = 0;
            this.sequence = 0;
        }
    }

    openEventSource() {
        if (!this.EventSourceCtor || !this.operationId) return;
        if (this.source) this.source.close();
        const source = new this.EventSourceCtor(this.api.OTA_EVENTS);
        this.source = source;
        source.addEventListener("open", () => {
            if (source !== this.source) return;
            this.onTransport("sse");
            this.retryDelayMs = this.pollIntervalMs;
            if (this.pollTimer != null) this.clearTimeoutFn(this.pollTimer);
            this.pollTimer = null;
        });
        source.addEventListener("ota-progress", event => {
            if (source !== this.source) return;
            try {
                this.acceptSnapshot(JSON.parse(event.data || "{}"));
            } catch (_) {
                // A later current snapshot can recover from malformed transport data.
            }
        });
        source.addEventListener("error", () => {
            if (source !== this.source) return;
            source.close();
            this.source = null;
            this.onTransport("polling");
            this.pollNow();
            this.scheduleSseRetry();
        });
    }

    scheduleSseRetry() {
        if (this.retryTimer != null || !this.operationId) return;
        const delay = this.retryDelayMs;
        this.retryDelayMs = Math.min(this.retryMaximumMs, Math.max(this.pollIntervalMs, delay * 2));
        this.retryTimer = this.setTimeoutFn(() => {
            this.retryTimer = null;
            if (this.operationId && !this.source) this.openEventSource();
        }, delay);
    }

    schedulePoll() {
        if (this.pollTimer != null || !this.operationId || this.source) return;
        this.pollTimer = this.setTimeoutFn(() => {
            this.pollTimer = null;
            this.pollNow();
        }, this.pollIntervalMs);
    }

    async pollNow() {
        if (!this.operationId || this.pollInFlight || this.source?.readyState === 1) return;
        this.pollInFlight = true;
        try {
            const response = await this.fetchFn(this.api.OTA_STATUS, {cache: "no-store"});
            const snapshot = await this.responseJson(response);
            this.acceptSnapshot(snapshot);
        } catch (_) {
            // Status is retried while the operation remains active.
        } finally {
            this.pollInFlight = false;
            this.schedulePoll();
        }
    }

    acceptSnapshot(snapshot) {
        const operationId = Number(snapshot?.operationId);
        const sequence = Number(snapshot?.sequence);
        if (!Number.isInteger(operationId) || !Number.isInteger(sequence) || operationId <= 0 || sequence <= 0) {
            return false;
        }
        if (this.operationId && operationId !== this.operationId) return false;
        if (!this.operationId) this.operationId = operationId;
        if (sequence <= this.sequence) return false;
        this.sequence = sequence;
        this.onSnapshot(snapshot);

        if (snapshot.state === "awaiting_upload" && this.awaitingUpload) {
            this.awaitingUpload.resolve();
            this.awaitingUpload = null;
        } else if (snapshot.state === "failed") {
            if (this.awaitingUpload) {
                this.awaitingUpload.reject(new Error(snapshot.error || "ota_failed"));
                this.awaitingUpload = null;
            }
            this.invalidateSession();
            this.stopMonitoring();
            this.resumeNormalStream();
        } else if (snapshot.state === "rebooting") {
            this.invalidateSession();
            this.stopMonitoring();
            this.onReboot(snapshot);
        }
        return true;
    }

    waitForAwaitingUpload() {
        if (this.awaitingUpload) return this.awaitingUpload.promise;
        let resolve;
        let reject;
        const promise = new Promise((accept, decline) => {
            resolve = accept;
            reject = decline;
        });
        this.awaitingUpload = {promise, resolve, reject};
        return promise;
    }

    async recoverAfterInitiationFailure() {
        try {
            const response = await this.fetchFn(this.api.OTA_STATUS, {cache: "no-store"});
            const snapshot = await this.responseJson(response);
            if (!ACTIVE_STATES.has(snapshot?.state) || !Number.isInteger(snapshot?.operationId)
                || snapshot.operationId <= 0) return false;
            this.monitor(snapshot.operationId);
            this.acceptSnapshot(snapshot);
            return true;
        } catch (_) {
            return false;
        }
    }
}
