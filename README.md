# Modbus-to-X

An ESP32-based hardware and firmware stack that bridges RS-485/Modbus field devices to modern network services such as MQTT, with a configuration-first UX for installers and maintainers.

## Table of Contents
- [Overview](#overview)
- [Architecture](#architecture)
  - [Hardware](#hardware)
  - [Firmware](#firmware)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Build and Flash](#build-and-flash)
  - [Release Automation](#release-automation)
  - [Local UI Development](#local-ui-development)
  - [HTTP API](#http-api)
- [Usage](#usage)
  - [First Boot and Network Setup](#first-boot-and-network-setup)
  - [Configuring Modbus Devices](#configuring-modbus-devices)
  - [MQTT Publishing](#mqtt-publishing)
  - [OTA Updates](#ota-updates)
- [Project Structure](#project-structure)
  - [Hardware Documentation](#hardware-documentation)
  - [Software Documentation](#software-documentation)
- [Acknowledgements](#acknowledgements)

## Overview
Modbus-to-X combines a custom ESP32 carrier board with firmware that polls Modbus RTU devices over RS-485, persists telemetry locally, and republishes structured payloads to MQTT brokers. 
The firmware exposes an onboard web interface for Wi-Fi provisioning, Modbus configuration, live device statistics, and log retrieval so field technicians can deploy the gateway without recompiling binaries.

## Architecture
The project is split between dedicated hardware design files and a PlatformIO firmware project. Hardware-specific documentation lives in [`hardware/`](hardware/); firmware, tooling, and UI assets live in [`software/`](software/).

### Hardware
- Four-layer ESP32 carrier featuring an ESP32-WROOM-32D module, MAX3485 RS-485 transceiver
- RJ12 Connector exposing differential RS-485 pair alongside field power for quick deployment.
- Wide input voltage range (4.5 - 40V)
- Supporting assets include datasheets, fabrication rules, and release notes for each board revision under `hardware/`.

### Firmware
- PlatformIO-based ESP32 application primarily built with the Arduino framework.
- Library dependencies: ModbusMaster, PubSubClient, ArduinoJson, and ESPAsyncWebServer for protocol handling and a dynamic UI backend.
- A custom partition table separates user configurations from UI components, leaving user configurations untouched on filesystem uploads.
- Boot sequence mounts filesystem partitions, starts the async web server ("MBX Server"), initializes the Modbus scheduler, and spins up the MQTT manager.
- Configuration data for the Modbus bus, devices, and MQTT settings is stored in `/conf/*.json` on the dedicated config SPIFFS partition (`cfg`) and hot-reloaded without reflashing.
- MQTT passwords are stored in NVS preferences; `/conf/mqtt.json` contains non-sensitive fields.
- Default wiring, RS-485 guard times, Wi-Fi AP Portal credentials, and OTA parameters are centralized in [`Config.h`](software/modbus-to-mqtt/include/Config.h).
- Signed and manual HTTP updates use a bounded quiescence coordinator and a compiled recovery page; the lifecycle and protocol are documented in [`OTA_LIFECYCLE.md`](OTA_LIFECYCLE.md).

## Getting Started

### Prerequisites
- Modbus-to-X hardware (v0.2 alpha or later) and Modbus RTU devices wired to the RS-485 terminals.
- 4.5–40 VDC field power (buck input validated in the current design) and a USB-to-UART adapter if not using OTA uploads.
- Development host with Python 3, [PlatformIO Core](https://platformio.org/install/cli), and a working `git` toolchain. Required Python build dependencies are installed automatically by `scripts/install_build_deps.py` when PlatformIO runs.

### Build and Flash
1. Clone the repository and enter the firmware workspace:
   ```bash
   git clone https://github.com/joachimda/modbus-to-x.git
   cd modbus-to-x/software/modbus-to-mqtt
   ```
2. Choose an environment in `platformio.ini`:
   - `mbx-custom-board-dev` — development build, serial upload (default).
   - `mbx-custom-board-dev-ota` — development build, OTA upload (configure `upload_port` for your device).
   - `mbx-custom-board-release` — release build (used by the GitHub Actions workflow).
   - `mbx-custom-board-release-primer` — primes a dev device to receive HTTP OTA updates.
3. Build the firmware (PlatformIO will fetch dependencies and embed git metadata):
   ```bash
   pio run
   ```
4. Flash via USB/UART or OTA by selecting the matching environment:
   ```bash
   # Serial upload
   pio run -e mbx-custom-board-dev -t upload

   # OTA upload to a provisioned gateway
   pio run -e mbx-custom-board-dev-ota -t upload
   ```
5. Upload the web UI and config assets to SPIFFS when they change:
   ```bash
   pio run -e mbx-custom-board-dev -t uploadfs
   ```

### Release Automation
Pushing a `v*` tag triggers the GitHub Actions workflow in `.github/workflows/main.yml` to build firmware + filesystem images and create a GitHub Release with the assets:
- `modbus-to-x.bin` (firmware)
- `modbus-to-x.fs.bin` (filesystem)
- `modbus-to-x.notes.txt` (release notes generated from the commit log since the previous tag)
- `modbus-to-x.manifest.json` (includes SHA-256 hashes and a signature generated from `OTA_SIGNING_KEY_PEM` and `KID`).

### Local UI Development
The `scripts/run_test_webserver.py` helper serves the `data/` directory with mocked API responses so you can iterate on the HTML/JS bundle without flashing the device:
```bash
cd software/modbus-to-mqtt
python scripts/run_test_webserver.py
# Visit http://127.0.0.1:8000
```
It injects correct MIME types, disables caching, and emulates the SPA's system, OTA, and administrative API behavior, including mutation-request protection.

Use `--provisioning-scenario delayed`, `temporary`, `completion-unavailable`, `readiness-html`, or another listed scenario to exercise captive handoff failures. Running the mock on port 80 allows its loopback station address to complete the full dashboard handoff.

Use `--ota-scenario success`, `failure`, or `sse-loss` to exercise OTA
admission, lifecycle progress, status-poll fallback, recovery, and reboot UI.

### HTTP API

See the [HTTP API request contract](HTTP_API.md) for the route and method matrix, browser cross-site protections, OTA authorization behavior, and non-browser client examples.

## Usage

### First Boot and Network Setup
1. Power the gateway; it will advertise a Wi-Fi AP using the defaults defined in `Config.h` (`MODBUS-MQTT-BRIDGE` / `you-shall-not-pass`).
2. Connect to the AP. The ESP will try getting 192.168.4.1 from DHCP. If supported on your OS, you will be prompted to open the Captive Portal and redirected automatically. If these fail, check your router for the device's IP address, and use your browser to open the configuration page `http://192.168.4.1/`. 
3. Follow the on-screen guidance to scan, join, or provision hidden networks, optionally specifying static IP parameters before saving.
4. After a saved network connection succeeds, the portal requests an acknowledged reboot, waits for the device to return in station mode, and opens the dashboard at the reported address. Temporary connections remain in the portal until a saved attempt succeeds.

### Configuring Modbus Devices
- Use **Configure Modbus** to edit the RS-485 bus, add devices, and define datapoints. Modbus configurations are stored in the config partition at `/conf/config.json` and can be applied live without rebooting.
- Configuration files follow the schema in `software/modbus-to-mqtt/data/conf/schema.json`; the excerpt below shows the datapoint fields. Each datapoint specifies the function code, register address, data type, scale, and engineering units that will be published when polled. Registers are decoded by `dataType` before the scale is applied: `int16`/`uint16` use one register (optionally a byte via `registerSlice`), `int32`/`uint32`/`float32` use two and `int64`/`uint64` four registers, in the `wordOrder` `big` (default, first register = most significant word) or `little`. `numOfRegisters` below the type's width is raised to it when the configuration loads.
- Per-device MQTT publishing and Home Assistant discovery can be toggled in the Modbus config; discovery publishes retained entity definitions for read/write datapoints.
  Example (excerpt):
  ```json
  {
    "devices": [
      {
        "name": "Boiler",
        "slaveId": 10,
        "mqttEnabled": true,
        "homeassistantDiscoveryEnabled": true,
        "dataPoints": [
          {
            "id": "temp",
            "name": "Temperature",
            "function": 4,
            "address": 100,
            "numOfRegisters": 1,
            "dataType": "int16",
            "scale": 0.1,
            "unit": "C",
            "topic": "plant/boiler/temp_c"
          },
          {
            "id": "flow",
            "name": "Flow",
            "function": 3,
            "address": 200,
            "numOfRegisters": 2,
            "dataType": "float32",
            "wordOrder": "little",
            "scale": 1,
            "unit": "m3/h"
          }
        ]
      }
    ]
  }
  ```

### MQTT Publishing
- Configure broker host, port, credentials, and optional root topic via the **Configure MQTT** page or by editing `/conf/mqtt.json`. The firmware automatically extracts hostnames from URLs and persists the MQTT password in NVS preferences.
- When Modbus reads succeed, datapoint values are published to MQTT using either the exact per-datapoint topic override or the default pattern `<root>/<device>/<datapoint>` with slugified device and datapoint names.
- Published values are decoded by the datapoint's `dataType` (and `wordOrder` for multi-register types) before the scale is applied, then printed with as many decimals as the scale has (`0.01` → `-2.00`, `1` → `1234`, `0.5` → `12.5`); `float32` values print with 7 significant digits. Coils and discrete inputs publish `1`/`0`; `TEXT` datapoints publish the ASCII content of their registers. A read that returns fewer registers than the data type needs is logged and not published.
- A writable datapoint treats its configured `topic` as a base and subscribes only to `<base>/set`; an empty base uses the same default resolution as a readable datapoint before `/set` is appended. Commands must be published without MQTT retain. There is deliberately no subscription on the legacy base/state topic.
- Existing state consumers require no changes. Existing command publishers and automations must migrate from `<base>` to `<base>/set`; retained state left on `<base>` is safe because the gateway no longer subscribes there.
- Home Assistant sensors advertise the resolved state topic. Switches and numbers advertise the derived `/set` command topic and use a readable datapoint with the same resolved base as state feedback, or remain optimistic when no matching read exists. Number entities advertise the data type's range times the scale as `min`/`max` and the absolute scale as `step` (never below Home Assistant's minimum of `0.001`).
- FC5 coil commands accept only `0`, `1`, `false`, or `true` after surrounding whitespace is removed; the words are case-insensitive.
- FC6 and FC16 commands accept a complete decimal value with an optional sign, fraction, and decimal exponent. Examples include `12`, `+12.5`, `.5`, `1.`, `1e3`, and `-2.5E-2`. Hexadecimal, `NaN`, infinity, and partially numeric text are rejected.
- Holding-register commands use `engineering = raw × scale`, where `raw` has the datapoint's `dataType`: `int16`/`uint16` occupy one register (FC6 or FC16), `int32`/`uint32`/`float32` two registers and `int64`/`uint64` four registers, written with FC16 in the configured `wordOrder`. The command value is divided by the configured finite, non-zero scale, rejected if the unrounded result is outside the type's range, and otherwise rounded to the nearest raw integer with exact halves rounded upward; `float32` values are stored without rounding. Negative scales are supported. FC6 cannot write 32-bit or 64-bit types; such a configuration is rejected with `invalid_register_count`. 64-bit values beyond 2^53 lose precision in the division.
- Rejected MQTT commands do not reach the Modbus bus. Their warning contains the command topic, function, address, and a stable reason code, but never echoes the rejected payload.
- MQTT connectivity, Modbus statistics, and recent logs are visible on the dashboard.

### OTA Updates

- Release checks and notes retrieval leave normal services running. Applying a release or starting a manual upload first stops ordinary HTTP/SPIFFS activity, Modbus transactions, MQTT reconnect/publication work, and the normal statistics/log stream within bounded deadlines.
- When connected, MQTT publishes the configured retained availability payload before its graceful disconnect. A failed best-effort signal or an already-disconnected broker cannot delay entry indefinitely.
- The dashboard authenticates once into a short-lived RAM-only OTA session, then shows OTA-only SSE progress with public status polling as a fallback. Passwords and session tokens are not stored in the browser.
- Manual firmware and filesystem uploads use a preflight operation and wait for `awaiting_upload` before sending image data. A successful operation remains quiescent until reboot; a recoverable failure restores only services that were enabled beforehand.
- If the UI filesystem is unavailable, `/` serves a self-contained recovery page compiled into firmware. The single filesystem partition is not transactional, so an interrupted filesystem write can still require that recovery path.

See [OTA lifecycle and recovery](OTA_LIFECYCLE.md) and the [HTTP API request contract](HTTP_API.md) for exact waits, memory guards, headers, progress fields, and response codes.

## Project Structure
```
.
├── hardware/                 # Schematics, PCB layout, BOM assets, release notes
├── software/
│   └── modbus-to-mqtt/       # PlatformIO workspace with firmware, web UI, and tooling
└── README.md                 # This high-level overview
```

### Hardware Documentation
Detailed hardware notes, KiCad sources, datasheets, and per-revision release notes are kept in [`hardware/`](hardware/). Start with the latest folder under [`hardware/releases/`](hardware/releases/) (for example, `v0.2-alpha`) for the current board spin and consult the KiCad project in [`hardware/kicad_files/`](hardware/kicad_files/) for schematics, layout, and fabrication outputs.

### Software Documentation
The firmware entry point, reusable libraries, and static assets live under [`software/modbus-to-mqtt/`](software/modbus-to-mqtt/). Key references:
- [`src/`](software/modbus-to-mqtt/src/) – firmware modules for Modbus polling, MQTT connectivity, OTA, and the async web server.
- [`data/`](software/modbus-to-mqtt/data/) – web UI bundle and config schema served by the device (config data lives in the `/conf` partition at runtime).
- [`docs/configuration_examples/`](software/modbus-to-mqtt/docs/configuration_examples/) – JSON schemas and examples for Modbus and MQTT configuration.
- [`boards/`](software/modbus-to-mqtt/boards/) – custom PlatformIO board definition for the production hardware (ESP32, 16 MB flash).


## Acknowledgements
- Built on Espressif's ESP32 platform and the open-source libraries listed in `platformio.ini` (ModbusMaster, PubSubClient, ArduinoJson, ESPAsyncWebServer).
- Component footprints and 3D models sourced from vendor or third-party libraries included under `hardware/kicad_files/lib/` with accompanying license terms in `hardware/kicad_files/lib/licenses`.
