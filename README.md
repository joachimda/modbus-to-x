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
  - [Local UI Development](#local-ui-development)
- [Usage](#usage)
  - [First Boot and Network Setup](#first-boot-and-network-setup)
  - [Configuring Modbus Devices](#configuring-modbus-devices)
  - [MQTT Publishing](#mqtt-publishing)
- [Project Structure](#project-structure)
  - [Hardware Documentation](#hardware-documentation)
  - [Software Documentation](#software-documentation)
- [Contributing](#contributing)
- [Roadmap](#roadmap)
- [License](#license)
- [Acknowledgements](#acknowledgements)

## Overview
Modbus-to-X combines a custom ESP32 carrier board with firmware that polls Modbus RTU devices over RS-485, persists telemetry locally, and republishes structured payloads to MQTT brokers. The firmware exposes an onboard web interface for Wi-Fi provisioning, Modbus configuration, live device statistics, and log retrieval so field technicians can deploy the gateway without recompiling binaries.【F:software/modbus-to-mqtt/src/main.cpp†L17-L106】【F:software/modbus-to-mqtt/data/index.html†L20-L125】

## Architecture
The project is split between dedicated hardware design files and a PlatformIO firmware project. Hardware-specific documentation lives in [`hardware/`](hardware/); firmware, tooling, and UI assets live in [`software/`](software/).

### Hardware
- Four-layer ESP32 carrier featuring an ESP32-WROOM-32D module, MAX3485 RS-485 transceiver, on-board EEPROM, and ample test points in the v0.1 alpha spin.【F:hardware/releases/v0.1-alpha/release_notes.md†L1-L7】【F:hardware/kicad_files/modbus-to-x.kicad_sch†L2013-L2043】【F:hardware/kicad_files/modbus-to-x.kicad_sch†L2575-L2666】
- Terminal block breakouts route differential RS-485 pairs alongside field power for quick deployment.【F:hardware/kicad_files/modbus-to-x.kicad_sch†L3845-L3931】【F:hardware/kicad_files/modbus-to-x.kicad_sch†L10790-L10853】
- Supporting assets include datasheets, fabrication rules, and release notes for each board revision under `hardware/`.

### Firmware
- PlatformIO-based ESP32 application built with the Arduino framework plus ModbusMaster, PubSubClient, ArduinoJson, and ESPAsyncWebServer for protocol handling and a dynamic UI backend.【F:software/modbus-to-mqtt/platformio.ini†L1-L50】
- Boot sequence mounts SPIFFS, restores crash logs from RTC memory, starts the async web server ("MBX Server"), initializes the Modbus scheduler, and spins up the MQTT manager with log mirroring for diagnostics.【F:software/modbus-to-mqtt/src/main.cpp†L35-L106】
- Configuration data for the Modbus bus, devices, and MQTT settings is stored in `/conf/*.json` on SPIFFS and hot-reloaded without reflashing.【F:software/modbus-to-mqtt/src/ModbusManager.cpp†L138-L239】【F:software/modbus-to-mqtt/src/mqtt/MqttManager.cpp†L21-L99】
- Default wiring, watchdog behaviour, RS-485 guard times, Wi-Fi AP credentials, and OTA parameters are centralized in [`Config.h`](software/modbus-to-mqtt/include/Config.h).【F:software/modbus-to-mqtt/include/Config.h†L4-L116】

## Getting Started

### Prerequisites
- Modbus-to-X hardware (v0.1 alpha or later) and Modbus RTU devices wired to the RS-485 terminals.【F:hardware/releases/v0.1-alpha/release_notes.md†L1-L7】【F:hardware/kicad_files/modbus-to-x.kicad_sch†L3845-L3931】
- 7–24 VDC field power (buck input validated in the current design) and a USB-to-UART adapter if not using OTA uploads.
- Development host with Python 3, [PlatformIO Core](https://platformio.org/install/cli), and a working `git` toolchain. Required Python build dependencies are installed automatically by `scripts/install_build_deps.py` when PlatformIO runs.【F:software/modbus-to-mqtt/scripts/install_build_deps.py†L1-L32】

### Build and Flash
1. Clone the repository and enter the firmware workspace:
   ```bash
   git clone https://github.com/joachimda/modbus-to-x.git
   cd modbus-to-x/software/modbus-to-mqtt
   ```
2. Choose an environment in `platformio.ini`:
   - `development_devkit` targets an off-the-shelf ESP32-DevKit via serial flashing.
   - `prod-board-v0-1-alpha` targets the custom board with OTA uploads enabled by default.【F:software/modbus-to-mqtt/platformio.ini†L21-L49】
3. Build the firmware (PlatformIO will fetch dependencies and embed git metadata):
   ```bash
   pio run
   ```
4. Flash via USB/UART or OTA depending on the selected environment:
   ```bash
   # Serial upload (development board)
   pio run -e development_devkit -t upload

   # OTA upload to a provisioned gateway
   pio run -e prod-board-v0-1-alpha -t upload
   ```
5. Upload the web UI assets to SPIFFS when they change:
   ```bash
   pio run -e prod-board-v0-1-alpha -t uploadfs
   ```

### Local UI Development
The `scripts/run_test_webserver.py` helper serves the `data/` directory with mocked API responses so you can iterate on the HTML/JS bundle without flashing the device:
```bash
cd software/modbus-to-mqtt
python scripts/run_test_webserver.py
# Visit http://127.0.0.1:8000
```
It injects correct MIME types, disables caching, and emulates the `/api/stats/system`, `/api/logs`, and reboot endpoints used by the SPA.【F:software/modbus-to-mqtt/scripts/run_test_webserver.py†L1-L200】

## Usage

### First Boot and Network Setup
1. Power the gateway; it will advertise a Wi-Fi AP using the defaults defined in `Config.h` (`MODBUS-MQTT-BRIDGE` / `you-shall-not-pass`).【F:software/modbus-to-mqtt/include/Config.h†L100-L108】
2. Connect to the AP and browse to `http://192.168.4.1/` to access the dashboard.【F:software/modbus-to-mqtt/data/index.html†L20-L125】
3. Open **Configure Wi-Fi** to scan, join, or provision hidden networks, optionally specifying static IP parameters before saving.【F:software/modbus-to-mqtt/data/pages/configure_network.html†L18-L82】

### Configuring Modbus Devices
- Use **Configure Modbus** to edit the RS-485 bus, add devices, and define datapoints. Changes are saved to `/conf/config.json` and can be applied live without rebooting.【F:software/modbus-to-mqtt/data/pages/configure.html†L18-L142】【F:software/modbus-to-mqtt/src/ModbusManager.cpp†L138-L239】
- Configuration files follow the schema in `docs/configuration_examples/modbus.json` and the default example stored in SPIFFS (`data/conf/config.json`). Each datapoint specifies the function code, register address, data type, scale, and engineering units that will be published when polled.【F:software/modbus-to-mqtt/docs/configuration_examples/modbus.json†L1-L114】【F:software/modbus-to-mqtt/data/conf/config.json†L1-L55】

### MQTT Publishing
- Configure broker host, port, credentials, and optional root topic via the **Configure MQTT** page or by editing `/conf/mqtt.json`. The firmware automatically extracts hostnames from URLs and persists passwords in NVS preferences.【F:software/modbus-to-mqtt/data/conf/mqtt.json†L1-L6】【F:software/modbus-to-mqtt/src/mqtt/MqttManager.cpp†L40-L99】
- When Modbus reads succeed, datapoint values are published to MQTT using either the configured topic override or the default pattern `<root>/<device>/<datapointId>` with slugified IDs.【F:software/modbus-to-mqtt/src/ModbusManager.cpp†L660-L708】
- MQTT connectivity, Modbus statistics, and recent logs are visible on the dashboard and mirrored to RTC memory so failures survive resets.【F:software/modbus-to-mqtt/src/main.cpp†L45-L106】【F:software/modbus-to-mqtt/data/index.html†L67-L105】

## Project Structure
```
.
├── hardware/                 # Schematics, PCB layout, BOM assets, release notes
├── software/
│   └── modbus-to-mqtt/       # PlatformIO workspace with firmware, web UI, and tooling
└── README.md                 # This high-level overview
```

### Hardware Documentation
Detailed hardware notes, KiCad sources, datasheets, and per-revision release notes are kept in [`hardware/`](hardware/). Start with [`hardware/releases/v0.1-alpha/release_notes.md`](hardware/releases/v0.1-alpha/release_notes.md) for the current board spin and consult the KiCad project in [`hardware/kicad_files/`](hardware/kicad_files/) for schematics, layout, and fabrication outputs.【F:hardware/releases/v0.1-alpha/release_notes.md†L1-L7】【F:hardware/kicad_files/modbus-to-x.kicad_sch†L2013-L2043】

### Software Documentation
The firmware entry point, reusable libraries, and static assets live under [`software/modbus-to-mqtt/`](software/modbus-to-mqtt/). Key references:
- [`src/`](software/modbus-to-mqtt/src/) – firmware modules for Modbus polling, MQTT connectivity, OTA, and the async web server.
- [`data/`](software/modbus-to-mqtt/data/) – SPIFFS web UI bundle served by the device.【F:software/modbus-to-mqtt/data/index.html†L1-L125】
- [`docs/configuration_examples/`](software/modbus-to-mqtt/docs/configuration_examples/) – JSON schemas and examples for Modbus and MQTT configuration.【F:software/modbus-to-mqtt/docs/configuration_examples/modbus.json†L1-L114】【F:software/modbus-to-mqtt/docs/configuration_examples/mqtt.json†L1-L5】
- [`boards/`](software/modbus-to-mqtt/boards/) – custom PlatformIO board definition for the production hardware (ESP32, 16 MB flash).【F:software/modbus-to-mqtt/boards/mbx_prod_board_v0_1_alpha.json†L1-L37】

## Contributing
1. Fork the repository and create feature branches per change.
2. For firmware work, keep PlatformIO environments focused on specific hardware targets and document any new dependencies in `platformio.ini` or the `lib/` folder.
3. Hardware changes should include updated release notes under `hardware/releases/<version>/` and, when applicable, regenerated fabrication outputs.
4. Run formatting and smoke tests relevant to your change (e.g., `pio check`, unit tests, or local UI validation) before opening a pull request.
5. Submit a PR describing the motivation, testing performed, and any cross-impact between hardware and software.

## Roadmap
- Harden the Modbus scheduler with configurable polling intervals and back-off strategies beyond the current fixed delay loop.【F:software/modbus-to-mqtt/src/main.cpp†L102-L105】
- Expand automated tests or CI smoke checks for configuration parsing and MQTT publishing logic.【F:software/modbus-to-mqtt/src/ModbusManager.cpp†L150-L231】【F:software/modbus-to-mqtt/src/mqtt/MqttManager.cpp†L40-L139】
- Document enclosure, power budget, and installation best practices alongside future hardware revisions in `hardware/releases/`.

## License
A top-level license has not yet been published. Hardware library assets include vendor-specific usage terms in `hardware/kicad_files/lib/licenses/`, and UI components reference MIT-licensed snippets in the HTML/CSS sources.【F:software/modbus-to-mqtt/data/pages/configure.html†L26-L57】【F:hardware/kicad_files/lib/licenses/M50-3140545_asm_License.txt†L1-L40】 Please open an issue if you need explicit licensing guidance before contributing.

## Acknowledgements
- Built on Espressif's ESP32 platform and the open-source libraries listed in `platformio.ini` (ModbusMaster, PubSubClient, ArduinoJson, ESPAsyncWebServer).【F:software/modbus-to-mqtt/platformio.ini†L15-L19】
- Web UI incorporates community design elements credited in `data/pages/configure.html`.【F:software/modbus-to-mqtt/data/pages/configure.html†L26-L57】
- Component footprints and 3D models sourced from vendor libraries included under `hardware/kicad_files/lib/` with accompanying license terms.【F:hardware/kicad_files/lib/licenses/Wurth_3P_Screw_Terminal_3.5mm_691214110003.txt†L1-L40】
