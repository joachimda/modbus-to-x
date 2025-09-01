import {safeJson} from "app";

window.initConfigureV2 = async function initConfigureV2() {
    await load().catch(err);
}

let model = { buses: [] };
let BUS_ID = "bus_1";
let selection = { kind: "bus", deviceId: null, datapointId: null };

async function load() {
    const cfg = await safeJson("/conf/config.json");
    if (cfg && Array.isArray(cfg.buses) && cfg.buses.length > 0) {
        model = { buses: [ cfg.buses[0] ] }; // ignore any extra buses just in case
        BUS_ID = model.buses[0].id || BUS_ID;
    } else {
        model = {
            buses: [{
                id: 1,
                name: "RS485 Bus",
                baud: 9600,
                parity: "N",
                stop_bits: 1,
                data_bits: 8,
                devices: []
            }]};
    }
    selection = {
        kind: "bus", deviceId: null, datapointId: null
    };
    buildTree();
    showBusEditor();
}

async function saveApply() {
    await safeJson("/api/config/modbus", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(model),
    });
    await safeJson("/api/modbus/apply", { method: "POST" });
    toast("Saved and applied.");
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
        type: "u16",
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
        <td><button class="btn" data-dp="${p.id}">Edit</button></td>
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
    $("#dp-type").value = datapoint.type || "u16";
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
        datapoint.func = $("#dp-func").value;
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
        const bus = Array.isArray(json.buses) ? json.buses[0] : null;
        if (!bus)
        {
            throw new Error("Import must contain a 'buses' array with a single bus.");
        }
        model = {
            buses: [ bus ]
        };
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
        const res = await safeJson("/api/config/validate", {
            method: "POST", headers: { "Content-Type":"application/json" },
            body: JSON.stringify({ buses: [ getBus() ] }),
        });
        alert(res.ok ? "Validation OK" : `Validation errors:\n${(res.errors||[]).join("\n")}`);
    } catch (e) { err(e); }
};
$("#btn-export").onclick = () => {
    const blob = new Blob([JSON.stringify({ buses: [ getBus() ] }, null, 2)], { type: "application/json" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "modbus-config.json";
    a.click();
    URL.revokeObjectURL(a.href);
};
$("#btn-reload").onclick = () => load();
$("#btn-save-apply").onclick = () => saveApply().catch(err);
