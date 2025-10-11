import {API, safeJson, STATIC_FILES} from "app";

window.initConfigure = async function initConfigure() {
    await load().catch(err);
}

let model = { buses: [] };
let BUS_ID = "bus_1";
let selection = { kind: "bus", deviceId: null, datapointId: null };

/* *
* * Mapping helpers between UI model and schema model
* * */
const SERIAL_FORMATS = new Set(["7N1","7N2","7O1","7O2","7E1","7E2","8N1","8N2","8E1","8E2","8O1","8O2"]);
const REGISTER_SLICES = new Set(["full","low_byte","high_byte"]);
function toSerialParts(fmt) {
    const def = { data_bits: 8, parity: "N", stop_bits: 1 };
    if (!fmt || typeof fmt !== "string" || fmt.length < 3) return def;
    const db = Number(fmt[0]);
    const p = fmt[1];
    const sb = Number(fmt[2]);
    if ((db === 7 || db === 8) && (p === "N" || p === "E" || p === "O") && (sb === 1 || sb === 2)) {
        return { data_bits: db, parity: p, stop_bits: sb };
    }
    return def;
}
function toSerialFormat(db, p, sb) {
    const dbn = Number(db);
    const sbn = Number(sb);
    const pr = (p || "N").toUpperCase();
    const fmt = `${(dbn===7||dbn===8)?dbn:8}${["N","E","O"].includes(pr)?pr:"N"}${(sbn===1||sbn===2)?sbn:1}`;
    return SERIAL_FORMATS.has(fmt) ? fmt : "8N1";
}
function schemaToUi(json) {
    // Expect current schema shape: { version, bus, devices }
    const parts = toSerialParts(json?.bus?.serialFormat);
    const bus = {
        id: 1,
        name: "RS485 Bus",
        baud: Number(json?.bus?.baud) || 9600,
        parity: parts.parity,
        stop_bits: parts.stop_bits,
        data_bits: parts.data_bits,
        devices: []
    };
    // devices
    const inDevices = Array.isArray(json?.devices) ? json.devices : [];
    bus.devices = inDevices.map((d, idx) => ({
        id: (typeof d.id === "string" && d.id.trim().length) ? d.id.trim() : `dev_${idx+1}`,
        name: (typeof d.name === "string" && d.name.trim().length) ? d.name.trim() : "device",
        slaveId: Number(d.slaveId) || 1,
        notes: (typeof d.notes === "string") ? d.notes : "",
        mqttEnabled: Boolean(d.mqttEnabled),
        homeassistantDiscoveryEnabled: Boolean(d.homeassistantDiscoveryEnabled),
        datapoints: Array.isArray(d.dataPoints) ? d.dataPoints.map((p) => {
            const rawAddress = p.address;
            const inferredFormat = (typeof rawAddress === "string" && /^0x/i.test(rawAddress.trim())) ? "hex" : "dec";
            return {
                id: p.id,
                name: p.name,
                func: Number(p.function ?? 3),
                address: toModbusAddress(rawAddress),
                addrFormat: inferredFormat,
                slice: normalizeRegisterSlice(p.registerSlice),
                length: Number(p.numOfRegisters ?? 1) || 1,
                type: String(p.dataType || "uint16"),
                scale: Number(p.scale ?? 1) || 1,
                unit: (typeof p.unit === "string") ? p.unit : "",
                topic: (typeof p.topic === "string") ? p.topic.trim() : "",
                poll_secs: (Number.isFinite(Number(p?.poll_interval))
                    ? Number(p.poll_interval)
                    : (Number.isFinite(Number(p?.poll_interval_ms)) ? Math.round(Number(p.poll_interval_ms)/1000) : 0))
            };
        }) : []
    }));

    return { buses: [ bus ] };
}
function uiToSchema(uiModel) {
    const b = (uiModel?.buses||[])[0] || {};
    return {
        version: 1,
        bus: {
            baud: Number(b.baud) || 9600,
            serialFormat: toSerialFormat(b.data_bits, b.parity, b.stop_bits)
        },
        devices: (b.devices || []).map(d => {
            const deviceId = (typeof d.id === "string" && d.id.trim().length) ? d.id.trim() : "";
            const device = {
                name: (typeof d.name === "string" && d.name.trim().length) ? d.name.trim() : "device",
                slaveId: Number(d.slaveId) || 1
            };
            if (deviceId.length) {
                device.id = deviceId;
            }
            if (d.mqttEnabled) {
                device.mqttEnabled = true;
            }
            if (d.homeassistantDiscoveryEnabled) {
                device.homeassistantDiscoveryEnabled = true;
            }
            device.dataPoints = (d.datapoints || []).map(p => {
                const topic = (typeof p.topic === "string") ? p.topic.trim() : "";
                const slice = normalizeRegisterSlice(p.slice);
                const dp = {
                    id: p.id,
                    name: p.name,
                    function: Number(p.func) || 3,
                    address: toModbusAddress(p.address),
                    numOfRegisters: Number(p.length) || 1,
                    dataType: String(p.type || "uint16"),
                    scale: Number(p.scale ?? 1) || 1,
                    unit: p.unit || "",
                    ...(Number.isFinite(Number(p.poll_secs)) && Number(p.poll_secs) > 0 ? { poll_interval: Number(p.poll_secs) } : {}),
                    ...(p.precision != null ? {
                        precision: Number(p.precision)
                    } : {})
                };
                if (slice !== "full") {
                    dp.registerSlice = slice;
                }
                if (topic.length) {
                    dp.topic = topic;
                }
                return dp;
            });
            return device;
        })
    };
}
function validateSchemaConfig(cfg) {
    const errors = [];
    // bus
    if (!cfg.bus || typeof cfg.bus !== "object") {
        errors.push("Missing bus");
    }
    if (cfg.bus) {
        if (!Number.isInteger(cfg.bus.baud) || cfg.bus.baud <= 0) {
            errors.push("Bus baud must be a positive integer");
        }
        if (!SERIAL_FORMATS.has(cfg.bus.serialFormat)) {
            errors.push(`Invalid serialFormat ${cfg.bus.serialFormat}`);
        }
    }
    for (const [i,d] of (cfg.devices||[]).entries()) {
        if (!d.name) {
            errors.push(`Device #${i+1}: name required`);
        }

        if (!Number.isInteger(d.slaveId) || d.slaveId < 1 || d.slaveId > 247) {
            errors.push(`Device ${d.name||i+1}: slaveId 1-247`);
        }
        if (d.id != null && typeof d.id !== "string") {
            errors.push(`Device ${d.name||i+1}: id must be a string`);
        }
        if (d.mqttEnabled != null && typeof d.mqttEnabled !== "boolean") {
            errors.push(`Device ${d.name||i+1}: mqttEnabled must be boolean`);
        }
        if (d.homeassistantDiscoveryEnabled != null && typeof d.homeassistantDiscoveryEnabled !== "boolean") {
            errors.push(`Device ${d.name||i+1}: homeassistantDiscoveryEnabled must be boolean`);
        }
        for (const [j,p] of (d.dataPoints||[]).entries()) {
            if (!p.name) {
                errors.push(`Datapoint #${j+1} on ${d.name}: name required`);
            }
            if (!p.id) {
                errors.push(`Datapoint ${p.name||j+1} on ${d.name}: id required`);
            }
            if (p.registerSlice != null && !REGISTER_SLICES.has(String(p.registerSlice))) {
                errors.push(`Datapoint ${p.id}: registerSlice invalid`);
            }
            if (p.registerSlice && p.registerSlice !== "full" && Number(p.numOfRegisters) !== 1) {
                errors.push(`Datapoint ${p.id}: registerSlice requires numOfRegisters = 1`);
            }
            if (!Number.isInteger(p.function) || p.function < 1 || p.function > 6) {
                errors.push(`Datapoint ${p.id}: function 1-6`);
            }
            if (p.topic != null && typeof p.topic !== "string") {
                errors.push(`Datapoint ${p.id}: topic must be a string`);
            }
            if (typeof p.topic === "string" && p.topic.length > 128) {
                errors.push(`Datapoint ${p.id}: topic too long`);
            }
            if (!Number.isInteger(p.address) || p.address < 0 || p.address > 65535) {
                errors.push(`Datapoint ${p.id}: address 0-65535`);
            }
            if (p.poll_interval != null && (!Number.isInteger(p.poll_interval) || p.poll_interval < 0)) {
                errors.push(`Datapoint ${p.id}: poll_interval must be >= 0 seconds`);
            }
            if (!Number.isInteger(p.numOfRegisters) || p.numOfRegisters < 1 || p.numOfRegisters > 125) {
                errors.push(`Datapoint ${p.id}: numOfRegisters 1-125`);
            }
            if (typeof p.unit === "string" && p.unit.length > 5) {
                errors.push(`Datapoint ${p.id}: unit max length 5`);
            }
            if (typeof p.name === "string" && p.name.length > 16) {
                errors.push(`Datapoint ${p.id}: name max length 16`);
            }
        }
    }
    return { ok: errors.length === 0, errors };
}

async function load() {
    const cfg = await safeJson(STATIC_FILES.MODBUS_CONFIG_JSON);
    model = schemaToUi(cfg);
    BUS_ID = (model.buses?.[0]?.id) || BUS_ID;
    selection = {
        kind: "bus", deviceId: null, datapointId: null
    };
    buildTree();
    showBusEditor();
}

async function doSaveApply(cfg) {
    await safeJson(API.PUT_MODBUS_CONFIG, {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(cfg),
    });
    toast("Saved and applied.");
}

function openConfirmModal() {
    const cfg = uiToSchema(model);
    const v = validateSchemaConfig(cfg);
    if (!v.ok) {
        alert(`Fix errors before saving:\n${v.errors.join("\n")}`);
        return;
    }
    // fill preview
    const pre = $("#json-preview");
    if (pre) pre.textContent = JSON.stringify(cfg, null, 2);
    // show modal
    const modal = $("#modal-confirm");
    modal.style.display = "flex";

    // wire buttons
    const approve = $("#btn-approve-apply");
    const cancel = $("#btn-cancel-apply");
    const closeBtn = $("#btn-confirm-close");
    const dl = $("#btn-download-backup");
    const close = () => { modal.style.display = "none"; };

    approve.onclick = async () => {
        try {
            await doSaveApply(cfg);
            close();
        } catch (e) { err(e); }
    };
    cancel.onclick = close;
    closeBtn.onclick = close;
    dl.onclick = async () => {
        try {
            const r = await fetch(STATIC_FILES.MODBUS_CONFIG_JSON, {
                cache: 'no-store'
            });
            if (!r.ok) {
                throw new Error(`${r.status} ${r.statusText}`);
            }
            const blob = await r.blob();
            const a = document.createElement('a');
            const ts = new Date()
                .toISOString()
                .replace(/[:T]/g,'-')
                .replace(/\..+/, '');
            a.href = URL.createObjectURL(blob);
            a.download = `config-backup-${ts}.json`;
            a.click();
            URL.revokeObjectURL(a.href);
        } catch (e) { err(e); }
    };
}

function buildTree() {
    const q = $("#tree-search").value?.toLowerCase() || "";
    const list = $("#tree-list");
    list.innerHTML = "";

    const b = getBus();
    const busNode = document.createElement("div");
    busNode.className = "node" + (selection.kind==="bus" ? " active" : "");
    busNode.innerHTML =
        `<div>
            <strong>${b.name || "(bus)"} </strong>
            <span class="pill">${(b.devices?.length || 0)} dev</span>
         </div>
         <div class="meta">${b.baud} • ${b.data_bits}${b.parity}${b.stop_bits}</div>`;

    busNode.onclick = () => {
        selection = {
            kind:"bus", deviceId:null, datapointId:null
        };
        buildTree();
        showBusEditor();
    };

    list.appendChild(busNode);

    // Devices + datapoints
    (b.devices || []).forEach(d => {
        const devNode = document.createElement("div");
        devNode.className = "node" + (selection.kind==="device" && selection.deviceId===d.id ? " active" : "");
        devNode.style.marginLeft = "1rem";
        devNode.classList.add("device");
        devNode.innerHTML =
            `<div>
                <strong>${d.name}</strong>
                <span class="pill">ID ${d.slaveId}</span>
                <span class="pill"> Datapoints: ${d.datapoints?.length || 0}</span>
            </div>
            <div class="meta">${b.name}</div>
            <div class="inline-actions">
                <button class="btn icon" data-add-dp="${d.id}" title="Add datapoint">+</button>
            </div>`;

        devNode.onclick = () => {
            selection = {
                kind:"device", deviceId:d.id, datapointId:null
            };
            buildTree();
            showDeviceEditor();
        };

        if (matches(q, [d.name, d.slaveId, b.name]))
        {
            list.appendChild(devNode);
        }

        devNode.querySelector('[data-add-dp]')?.addEventListener('click', (ev) => {
            ev.stopPropagation();
            selection = {
                kind:"device", deviceId: d.id, datapointId: null
            };
            addDatapointToCurrentDevice();
        });

        (d.datapoints || []).forEach(p => {
            const dpNode = document.createElement("div");
            dpNode.className = "node" + (selection.kind==="dp" && selection.datapointId===p.id ? " active" : "");
            dpNode.style.marginLeft = "2rem";
            dpNode.innerHTML =
                `<div class="mono">${p.id}</div>
                 <div class="meta">${p.name} • F${p.func} @ 0x${numberToHex(p.address)}</div>`;
            dpNode.onclick = () => {
                selection = {
                    kind:"dp", deviceId:d.id, datapointId:p.id
                };
                buildTree();
                showDatapointEditor();
            };
            if (matches(q, [p.id, p.name, p.func, p.address]))
            {
                list.appendChild(dpNode);
            }
        });
    });
}

function addDatapointToCurrentDevice() {
    const d = getDevice(selection.deviceId);
    if (!d) return;
    const name = "datapoint";
    const baseId = dpIdFrom(d.name || "device", name);
    let unique = baseId, i = 2;
    while (findDpById(unique)) unique = `${baseId}_${i++}`;
    d.datapoints = d.datapoints || [];
    d.datapoints.push({
        id: unique, name,
        func: 3,
        address: 0,
        addrFormat: "dec",
        slice: "full",
        length: 1,
        type: "uint16",
        scale: 1,
        unit: "",
        topic: "",
        poll_secs: 0
    });
    selection = {
        kind:"dp", deviceId:d.id, datapointId: unique
    };
    buildTree();
    showDatapointEditor();
}

// --- editors ---
function showBusEditor() {
    const b = getBus();
    if (!b)
    {
        return;
    }
    showOnly("#editor-bus");
    $("#bus-baud").value = b.baud || 9600;
    $("#bus-serial-parity").value = b.parity || "N";
    $("#bus-serial-stop-bits").value = b.stop_bits || 1;
    $("#bus-data-bits").value = b.data_bits || 8;
    $("#btn-bus-save").onclick = () => {
        b.baud = Number($("#bus-baud").value);
        b.data_bits = $("#bus-data-bits").value;
        b.parity = $("#bus-serial-parity").value;
        b.stop_bits = $("#bus-serial-stop-bits").value;
        buildTree();
        toast("Bus updated (draft)");
    };
}

function showDeviceEditor() {
    const bus = getBus();
    const device = getDevice(selection.deviceId);
    if (!bus || !device)
    {
        return;
    }

    showOnly("#editor-device");

    $("#dev-name").value = device.name || "";
    $("#dev-slave").value = device.slaveId ?? 1;
    $("#dev-notes").value = device.notes || "";
    const mqttToggle = $("#dev-mqtt");
    if (mqttToggle) {
        mqttToggle.checked = Boolean(device.mqttEnabled);
    }

    const haToggle = $("#dev-ha-discovery");
        if (haToggle) {
        haToggle.checked = Boolean(device.homeassistantDiscoveryEnabled);
    }

    renderDeviceDatapointTable(device);

    $("#btn-dev-save").onclick = () => {
        device.name = $("#dev-name").value.trim() || "device";
        device.slaveId = Number($("#dev-slave").value);
        device.notes = $("#dev-notes").value.trim();
        if (mqttToggle) {
            device.mqttEnabled = mqttToggle.checked;
        }
        if (haToggle) {
            device.homeassistantDiscoveryEnabled = haToggle.checked;
        }
        buildTree();
        toast("Device updated (draft)");
    };

    $("#btn-dev-delete").onclick = () => {
        if (!confirm("Delete this device and its datapoints?"))
        {
            return;
        }

        bus.devices = (bus.devices || []).filter(x => x.id !== device.id);
        selection = {
            kind:"bus", deviceId:null, datapointId:null
        };
        buildTree();
        showBusEditor();
    };
}

function renderDeviceDatapointTable(d) {
    const tbody = $("#dev-dp-table tbody");
    tbody.innerHTML = (d.datapoints || []).map(p => `
      <tr>
        <td class="mono">${p.id}</td>
        <td>${p.name}</td>
        <td>${p.func}</td>
        <td>${p.address}</td>
        <td>${p.type}</td>
        <td><button class="btn small" data-dp="${p.id}">Edit</button></td>
      </tr>
    `).join("");
    tbody.querySelectorAll("button[data-dp]").forEach(b =>
        b.addEventListener("click", () => {
            selection = {
                kind:"dp", deviceId: selection.deviceId, datapointId: b.dataset.dp
            };
            buildTree();
            showDatapointEditor();
        })
    );
}

function showDatapointEditor() {
    const device = getDevice(selection.deviceId);
    const datapoint = getDatapoint(selection.deviceId, selection.datapointId);
    if (!device || !datapoint)
    {
        return;
    }
    showOnly("#editor-dp");

    const devSel = $("#dp-device");
    const devices = (getBus()?.devices || []);
    devSel.innerHTML = devices.map(dev => `<option value="${dev.id}">${dev.name}</option>`).join("");
    devSel.value = device.id;

    $("#dp-name").value = datapoint.name || "";
    $("#dp-autoid").textContent = datapoint.id || "—";
    $("#dp-func").value = datapoint.func || 3;
    const addrInput = $("#dp-addr");
    const addrFormatInputs = document.querySelectorAll('input[name="dp-addr-format"]');
    let currentAddressFormat = datapoint.addrFormat === "hex" ? "hex" : "dec";
    datapoint.addrFormat = currentAddressFormat;
    const setAddressPlaceholder = () => {
        addrInput.placeholder = currentAddressFormat === "hex" ? "e.g. 0x10" : "e.g. 16";
    };
    addrFormatInputs.forEach(input => {
        input.checked = input.value === currentAddressFormat;
    });
    addrInput.value = addressInputValue(datapoint.address, currentAddressFormat);
    datapoint.addrFormat = currentAddressFormat;
    $("#dp-slice").value = datapoint.slice || "full";
    setAddressPlaceholder();
    $("#dp-len").value = datapoint.length ?? 1;
    $("#dp-type").value = datapoint.type || "uint16";
    $("#dp-scale").value = datapoint.scale ?? 1;
    $("#dp-unit").value = datapoint.unit || "";
    $("#dp-topic").value = datapoint.topic || "";
    $("#dp-poll").value = Number.isFinite(Number(datapoint.poll_secs)) ? Number(datapoint.poll_secs) : 0;
    const updateTestValueVisibility = () => {
        const func = Number($("#dp-func").value);
        const isWrite = (func === 5 || func === 6);
        const input = $("#dp-test-value");
        const label = document.querySelector('label[for="dp-test-value"]');
        if (input) input.style.display = isWrite ? "" : "none";
        if (label) label.style.display = "none"; // keep label hidden; placeholder explains usage
    };
    updateTestValueVisibility();
    $("#dp-func").onchange = updateTestValueVisibility;

    const updateAddressField = (num) => {
        const sanitized = Number.isInteger(num) && num >= 0 ? num : 0;
        addrInput.value = addressInputValue(sanitized, currentAddressFormat);
    };
    addrFormatInputs.forEach(input => {
        input.onchange = () => {
            if (!input.checked) return;
            const prevFormat = currentAddressFormat;
            const parsed = parseAddressInput(addrInput.value, prevFormat);
            const fallback = Number.isInteger(parsed) && parsed >= 0
                ? parsed
                : (Number.isInteger(datapoint.address) ? datapoint.address : 0);
            currentAddressFormat = input.value === "hex" ? "hex" : "dec";
            datapoint.addrFormat = currentAddressFormat;
            updateAddressField(fallback);
            setAddressPlaceholder();
        };
    });

    $("#btn-dp-save").onclick = () => {
        const newDevId = $("#dp-device").value;
        const name = $("#dp-name").value.trim();
        const newId = dpIdFrom(getDevice(newDevId)?.name || "device", name || "datapoint");

        if (findDpById(newId) && newId !== datapoint.id) {
            alert(`Datapoint ID "${newId}" already exists.`);
            return;
        }

        // content
        datapoint.name = name || "datapoint";
        datapoint.func = Number($("#dp-func").value);
        const parsedAddress = parseAddressInput(addrInput.value, currentAddressFormat);
        if (!Number.isInteger(parsedAddress) || parsedAddress < 0) {
            const fmtLabel = currentAddressFormat === "hex" ? "hex" : "decimal";
            alert(`Address must be a valid non-negative ${fmtLabel} value.`);
            return;
        }
        datapoint.address = parsedAddress;
        datapoint.slice = normalizeRegisterSlice($("#dp-slice").value);
        datapoint.length = Number($("#dp-len").value);
        datapoint.type = $("#dp-type").value;
        datapoint.scale = Number($("#dp-scale").value);
        datapoint.unit = ($("#dp-unit").value || "").trim();
        datapoint.topic = ($("#dp-topic").value || "").trim();
        datapoint.poll_secs = Math.max(0, Number($("#dp-poll").value) || 0);

        // move and rename if a device changed
        if (newDevId !== selection.deviceId) {
            const oldDev = getDevice(selection.deviceId);
            oldDev.datapoints = (oldDev.datapoints || []).filter(x => x.id !== datapoint.id);
            const newDev = getDevice(newDevId);
            datapoint.id = newId;
            newDev.datapoints = newDev.datapoints || [];
            newDev.datapoints.push(datapoint);
            selection = {
                kind: "dp", deviceId: newDevId, datapointId: datapoint.id
            };
        }
        else {
            datapoint.id = newId;
        }

        buildTree();
        showDatapointEditor();
        toast("Datapoint updated (draft)");
    };
    $("#btn-dp-delete").onclick = () => {
        if (!confirm("Delete this datapoint?"))
        {
            return;
        }

        device.datapoints = (device.datapoints || [])
            .filter(x => x.id !== datapoint.id);

        selection = {
            kind:"device", deviceId: device.id, datapointId: null
        };

        buildTree();
        showDeviceEditor();
    };

    // Test command (read/write based on selected function)
    $("#btn-dp-test-read").onclick = async () => {
        const func = Number($("#dp-func").value);
        const addr = parseAddressInput(addrInput.value, currentAddressFormat);
        const len = Number($("#dp-len").value);
        const writeVal = $("#dp-test-value").value.trim();

        if (!Number.isInteger(addr) || addr < 0) {
            return alert("Invalid address");
        }
        if (!Number.isInteger(len) || len <= 0) {
            return alert("Invalid length");
        }

        const q = new URLSearchParams({
            devId: String(selection.deviceId),
            dpId: String(datapoint.id),
            func_code: String(func),
            addr: String(addr),
            len: String(len),
        });
        if ((func === 5 || func === 6) && writeVal.length) {
            q.set('value', writeVal);
        }

        const btn = $("#btn-dp-test-read");
        const resEl = $("#dp-test-result");
        btn.disabled = true;
        resEl.textContent = (func === 5 || func === 6) ? "Writing…" : "Reading…";
        try {
            const r = await safeJson(`${API.POST_MODBUS_EXECUTE}?${q.toString()}`, { method: 'POST' });
            const raw = r?.result?.raw;
            const value = (r?.result && (r.result.value ?? r.result?.raw?.[0])) ?? r.value ?? '(n/a)';

            let msg = `OK: ${value} ${datapoint.unit || ""}`;
            if (Array.isArray(raw)) {
                msg += ` raw=[${raw.join(', ')}]`;
            }
            resEl.textContent = msg;
        } catch (e) {
            resEl.textContent = `Error: ${e.message}`;
        } finally {
            btn.disabled = false;
        }
    };
    // Guard in case a write-only button exists in markup later
    const writeBtn = document.querySelector('#btn-dp-test-write');
    if (writeBtn) {
        writeBtn.onclick = () => document.querySelector('#btn-dp-test-read').click();
    }
}

/*
* Helpers
* */
const $ = (s) => document.querySelector(s);
const slug = (s) => (s || "")
    .toString().trim().toLowerCase()
    .replace(/[^a-z0-9]+/g, "_")
    .replace(/^_+|_+$/g,"");
const dpIdFrom = (deviceName, dpName) => `${slug(deviceName)}.${slug(dpName)}`;
const getBus = () => model.buses[0];
const getDevice = (did) => (getBus()?.devices || []).find(d => d.id === did);
const getDatapoint = (did, pid) => (getDevice(did)?.datapoints || []).find(p => p.id === pid);

function parseAddressInput(value, format) {
    const raw = (value ?? "").toString().trim();
    if (!raw.length) return NaN;

    const targetFormat = format === "hex" ? "hex" : (format === "dec" ? "dec" : null);
    if (targetFormat === "hex") {
        const noPrefix = raw.replace(/^0x/i, "");
        return /^[0-9a-f]+$/i.test(noPrefix) ? parseInt(noPrefix, 16) : NaN;
    }
    if (targetFormat === "dec") {
        return /^[0-9]+$/.test(raw) ? Number(raw) : NaN;
    }

    if (/^0x[0-9a-f]+$/i.test(raw)) {
        return parseInt(raw, 16);
    }
    if (/^[0-9]+$/.test(raw)) {
        return Number(raw);
    }
    if (/^[0-9a-f]+$/i.test(raw)) {
        return parseInt(raw, 16);
    }

    const num = Number(raw);
    return Number.isFinite(num) ? Math.trunc(num) : NaN;
}
function addressInputValue(value, format = "dec") {
    const parsedValue = Number.isFinite(Number(value)) ? Number(value) : parseAddressInput(value, format);
    if (!Number.isInteger(parsedValue) || parsedValue < 0) {
        return "";
    }
    const intVal = Math.trunc(parsedValue);
    return format === "hex"
        ? `0x${intVal.toString(16)}`
        : String(intVal);
}
function toModbusAddress(value) {
    const parsed = parseAddressInput(value);
    return Number.isInteger(parsed) && parsed >= 0 ? parsed : 0;
}

function normalizeRegisterSlice(value) {
    const raw = (value ?? "full").toString().trim().toLowerCase();
    if (raw === "full_register") {
        return "full";
    }
    if (raw === "1") {
        return "low_byte";
    }
    if (raw === "2") {
        return "high_byte";
    }
    if (REGISTER_SLICES.has(raw)) {
        return raw;
    }
    if (raw === "low" || raw === "lowbyte") {
        return "low_byte";
    }
    if (raw === "high" || raw === "highbyte") {
        return "high_byte";
    }
    return "full";
}

function toast(msg) {
    console.log(msg);
}

function err(e) {
    console.error(e);
    alert(e.message || e);
}

function findDpById(id) {
    const b = getBus();
    for (const d of (b.devices || [])) {
        for (const p of (d.datapoints || [])) {
            if (p.id === id) return p;
        }
    }
    return null;
}

const recomputeId = () => {
    const dev = getDevice($("#dp-device").value);
    const name = $("#dp-name").value;
    $("#dp-autoid").textContent = dpIdFrom(dev?.name || "device", name || "datapoint");
};
function showOnly(id) {
    $("#editor-placeholder").style.display = "none";
    ["#editor-bus","#editor-device","#editor-dp"].forEach(s => $(s).style.display = "none");
    $(id).style.display = "block";
}
function matches(q, arr) {
    if (!q)
    {
        return true;
    }
    return arr.some(x => (x+"").toLowerCase().includes(q));
}
function numberToHex(n) {
    if (n == null || Number.isNaN(Number(n))) return "";
    const num = Math.trunc(Number(n));
    return Math.abs(num).toString(16);
}

/*
* Listeners
* */
$("#file-import").addEventListener("change", async (ev) => {
    const f = ev.target.files?.[0];
    if (!f) return;
    const text = await f.text();
    try {
        const json = JSON.parse(text);
        const ui = schemaToUi(json);
        const bus = ui?.buses?.[0];
        if (!bus) {
            throw new Error("Invalid config file: missing bus");
        }
        model = ui;
        BUS_ID = bus.id || BUS_ID;
        selection = {
            kind:"bus", deviceId:null, datapointId:null
        };
        buildTree();
        showBusEditor();
        toast("Imported draft (not saved yet)");
    }
    catch (e) {
        err(e);
    }
});
$('#tree-search').addEventListener("input", buildTree);
$('#dp-name').addEventListener("input", recomputeId);
$('#dp-device').addEventListener("change", recomputeId);

$('#btn-add-device').onclick = () => {
    const b = getBus();
    const id = `dev_${Date.now()}`;
    b.devices = b.devices || [];
    b.devices.push({ id, name: "device", slaveId: 1, notes: "", mqttEnabled: false, homeassistantDiscoveryEnabled: false, datapoints: [] });
    selection = {
        kind:"device", deviceId:id, datapointId:null
    };
    buildTree();
    showDeviceEditor();
};
$("#btn-dev-add-dp").onclick = () => {
    if (selection.kind !== "device") {
        return;
    }
    addDatapointToCurrentDevice();
};

$("#btn-export").onclick = () => {
    const cfg = uiToSchema(model);
    const blob = new Blob([JSON.stringify(cfg, null, 2)], {
        type: "application/json"
    });

    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "modbus-config.json";
    a.click();
    URL.revokeObjectURL(a.href);
};
$("#btn-reload").onclick = () => load();
$("#btn-save-apply").onclick = () => openConfirmModal();
