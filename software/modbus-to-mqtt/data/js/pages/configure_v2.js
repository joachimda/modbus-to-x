import {safeJson} from "app";

window.initConfigureV2 = async function initConfigureV2() {
    await load().catch(err);
}

let model = { buses: [] };
let BUS_ID = "bus_1";
let selection = { kind: "bus", deviceId: null, datapointId: null };

// --- mapping helpers between UI model and schema model ---
const SERIAL_FORMATS = new Set(["7N1","7N2","7O1","7O2","7E1","7E2","8N1","8N2","8E1","8E2","8O1","8O2"]);
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
        id: d.id || `dev_${idx+1}`,
        name: d.name || "device",
        slaveId: Number(d.slaveId) || 1,
        notes: d.notes || "",
        datapoints: Array.isArray(d.dataPoints) ? d.dataPoints.map((p) => ({
            id: p.id,
            name: p.name,
            func: Number(p.function ?? 3),
            address: Number(p.address) || 0,
            length: Number(p.numOfRegisters ?? 1) || 1,
            type: String(p.dataType || "uint16"),
            scale: Number(p.scale ?? 1) || 1,
            unit: p.unit || "",
            topic: p.topic || ""
        })) : []
    }));

    return { buses: [ bus ] };
}
function uiToSchema(uiModel) {
    const b = (uiModel?.buses||[])[0] || {};
    const cfg = {
        version: 1,
        bus: {
            baud: Number(b.baud) || 9600,
            serialFormat: toSerialFormat(b.data_bits, b.parity, b.stop_bits)
        },
        devices: (b.devices||[]).map(d => ({
            name: d.name || "device",
            slaveId: Number(d.slaveId) || 1,
            dataPoints: (d.datapoints||[]).map(p => ({
                id: p.id,
                name: p.name,
                function: Number(p.func) || 3,
                address: Number(p.address) || 0,
                numOfRegisters: Number(p.length) || 1,
                dataType: String(p.type || "uint16"),
                scale: Number(p.scale ?? 1) || 1,
                unit: p.unit || "",
                ...(p.precision != null ? { precision: Number(p.precision) } : {})
            }))
        }))
    };
    return cfg;
}
function validateSchemaConfig(cfg) {
    const errors = [];
    // bus
    if (!cfg.bus || typeof cfg.bus !== "object") errors.push("Missing bus");
    if (cfg.bus) {
        if (!Number.isInteger(cfg.bus.baud) || cfg.bus.baud <= 0) errors.push("Bus baud must be a positive integer");
        if (!SERIAL_FORMATS.has(cfg.bus.serialFormat)) errors.push(`Invalid serialFormat ${cfg.bus.serialFormat}`);
    }
    // devices
    for (const [i,d] of (cfg.devices||[]).entries()) {
        if (!d.name) errors.push(`Device #${i+1}: name required`);
        if (!Number.isInteger(d.slaveId) || d.slaveId < 1 || d.slaveId > 247) errors.push(`Device ${d.name||i+1}: slaveId 1-247`);
        for (const [j,p] of (d.dataPoints||[]).entries()) {
            if (!p.name) errors.push(`Datapoint #${j+1} on ${d.name}: name required`);
            if (!p.id) errors.push(`Datapoint ${p.name||j+1} on ${d.name}: id required`);
            if (!Number.isInteger(p.function) || p.function < 1 || p.function > 6) errors.push(`Datapoint ${p.id}: function 1-6`);
            if (!Number.isInteger(p.address) || p.address < 0 || p.address > 65535) errors.push(`Datapoint ${p.id}: address 0-65535`);
            if (!Number.isInteger(p.numOfRegisters) || p.numOfRegisters < 1 || p.numOfRegisters > 125) errors.push(`Datapoint ${p.id}: numOfRegisters 1-125`);
            if (typeof p.unit === "string" && p.unit.length > 5) errors.push(`Datapoint ${p.id}: unit max length 5`);
            if (typeof p.name === "string" && p.name.length > 16) errors.push(`Datapoint ${p.id}: name max length 16`);
        }
    }
    return { ok: errors.length === 0, errors };
}

async function load() {
    const cfg = await safeJson("/conf/config.json");
    // Map any incoming shape to our UI model
    model = schemaToUi(cfg);
    BUS_ID = (model.buses?.[0]?.id) || BUS_ID;
    selection = {
        kind: "bus", deviceId: null, datapointId: null
    };
    buildTree();
    showBusEditor();
}

async function doSaveApply(cfg) {
    await safeJson("/api/config/modbus", {
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
            const r = await fetch('/conf/config.json', { cache: 'no-store' });
            if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
            const blob = await r.blob();
            const a = document.createElement('a');
            const ts = new Date().toISOString().replace(/[:T]/g,'-').replace(/\..+/, '');
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
    busNode.innerHTML = `
      <div><strong>${b.name || "(bus)"} </strong> <span class="pill">${(b.devices?.length || 0)} dev</span></div>
      <div class="meta">${b.baud} • ${b.data_bits}${b.parity}${b.stop_bits}</div>`;

    busNode.onclick = () => {
        selection = {
            kind:"bus", deviceId:null, datapointId:null
        };
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
        length: 1,
        type: "uint16",
        scale: 1,
        unit: "",
        topic: ""
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

    renderDeviceDatapointTable(device);

    $("#btn-dev-save").onclick = () => {
        device.name = $("#dev-name").value.trim() || "device";
        device.slaveId = Number($("#dev-slave").value);
        device.notes = $("#dev-notes").value.trim();
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
    $("#dp-addr").value = datapoint.address ?? 0;
    $("#dp-len").value = datapoint.length ?? 1;
    $("#dp-type").value = datapoint.type || "uint16";
    $("#dp-scale").value = datapoint.scale ?? 1;
    $("#dp-unit").value = datapoint.unit || "";
    $("#dp-topic").value = datapoint.topic || "";
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
        datapoint.address = Number($("#dp-addr").value);
        datapoint.length = Number($("#dp-len").value);
        datapoint.type = $("#dp-type").value;
        datapoint.scale = Number($("#dp-scale").value);
        datapoint.unit = ($("#dp-unit").value || "").trim();
        datapoint.topic = ($("#dp-topic").value || "").trim();

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

    // test read/write
    $("#btn-dp-test-read").onclick = async () => {
        $('#dp-test-result').textContent = "Reading…";
        try {
            const r = await safeJson(`/api/modbus/test-read?id=${encodeURIComponent(datapoint.id)}`);
            $("#dp-test-result").textContent = `Value: ${r.value} ${datapoint.unit || ""}`;
        } catch (e) {
            $("#dp-test-result").textContent = `Error: ${e.message}`;
        }
    };
    $("#btn-dp-test-write").onclick = async () => {
        const val = $("#dp-test-value").value.trim();
        if (!val) return alert("Provide a value to write");
        $('#dp-test-result').textContent = "Writing…";
        try {
            await safeJson(`/api/modbus/test-write`, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ id: datapoint.id, value: isNaN(Number(val)) ? val : Number(val) }),
            });
            $("#dp-test-result").textContent = "Write OK";
        } catch (e) {
            $("#dp-test-result").textContent = `Error: ${e.message}`;
        }
    };
}

/*
* Helpers
* */
const $ = (s) => document.querySelector(s);
const slug = (s) => (s || "").toString().trim().toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_+|_+$/g,"");
const dpIdFrom = (deviceName, dpName) => `${slug(deviceName)}.${slug(dpName)}`;
const getBus = () => model.buses[0];
const getDevice = (did) => (getBus()?.devices || []).find(d => d.id === did);
const getDatapoint = (did, pid) => (getDevice(did)?.datapoints || []).find(p => p.id === pid);

function toast(msg) { console.log(msg); }
function err(e) { console.error(e); alert(e.message || e); }
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
        if (!bus) throw new Error("Invalid config file: missing bus");
        model = ui;
        BUS_ID = bus.id || BUS_ID;
        selection = {
            kind:"bus", deviceId:null, datapointId:null
        };
        buildTree();
        showBusEditor();
        toast("Imported draft (not saved yet)");
    } catch (e) { err(e); }
});
$('#tree-search').addEventListener("input", buildTree);
$('#dp-name').addEventListener("input", recomputeId);
$('#dp-device').addEventListener("change", recomputeId);

$('#btn-add-device').onclick = () => {
    const b = getBus();
    const id = `dev_${Date.now()}`;
    b.devices = b.devices || [];
    b.devices.push({ id, name: "device", slaveId: 1, notes: "", datapoints: [] });
    selection = {
        kind:"device", deviceId:id, datapointId:null
    };
    buildTree();
    showDeviceEditor();
};
$("#btn-dev-add-dp").onclick = () => {
    if (selection.kind !== "device") return;
    addDatapointToCurrentDevice();
};
$("#btn-validate").onclick = async () => {
    try {
        const cfg = uiToSchema(model);
        const v = validateSchemaConfig(cfg);
        if (!v.ok) return alert(`Validation errors:\n${v.errors.join("\n")}`);
        const res = await safeJson("/api/config/validate", {
            method: "POST", headers: { "Content-Type":"application/json" },
            body: JSON.stringify(cfg),
        });
        alert(res.ok ? "Validation OK" : `Validation errors:\n${(res.errors||[]).join("\n")}`);
    } catch (e) { err(e); }
};
$("#btn-export").onclick = () => {
    const cfg = uiToSchema(model);
    const blob = new Blob([JSON.stringify(cfg, null, 2)], { type: "application/json" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "modbus-config.json";
    a.click();
    URL.revokeObjectURL(a.href);
};
$("#btn-reload").onclick = () => load();
$("#btn-save-apply").onclick = () => openConfirmModal();
