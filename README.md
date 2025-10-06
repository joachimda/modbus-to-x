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
- [Acknowledgements](#acknowledgements)

## Overview
Modbus-to-X combines a custom ESP32 carrier board with firmware that polls Modbus RTU devices over RS-485, persists telemetry locally, and republishes structured payloads to MQTT brokers. The firmware exposes an onboard web interface for Wi-Fi provisioning, Modbus configuration, live device statistics, and log retrieval so field technicians can deploy the gateway without recompiling binaries.

## Architecture
The project is split between dedicated hardware design files and a PlatformIO firmware project. Hardware-specific documentation lives in [`hardware/`](hardware/); firmware, tooling, and UI assets live in [`software/`](software/).

### Hardware
- Four-layer ESP32 carrier featuring an ESP32-WROOM-32D module, MAX3485 RS-485 transceiver
- Terminal block breakouts route differential RS-485 pairs alongside field power for quick deployment.
- Supporting assets include datasheets, fabrication rules, and release notes for each board revision under `hardware/`.

### Firmware
- PlatformIO-based ESP32 application primarily built with the Arduino framework.
- Library dependencies: ModbusMaster, PubSubClient, ArduinoJson, and ESPAsyncWebServer for protocol handling and a dynamic UI backend.
- Boot sequence mounts SPIFFS, starts the async web server ("MBX Server"), initializes the Modbus scheduler, and spins up the MQTT manager with log mirroring for diagnostics.
- Configuration data for the Modbus bus, devices, and MQTT settings is stored in `/conf/*.json` on SPIFFS and hot-reloaded without reflashing.
- Default wiring, watchdog behaviour, RS-485 guard times, Wi-Fi AP credentials, and OTA parameters are centralized in [`Config.h`](software/modbus-to-mqtt/include/Config.h).

## Getting Started

### Prerequisites
- Modbus-to-X hardware (v0.1 alpha or later) and Modbus RTU devices wired to the RS-485 terminals.
- 7–24 VDC field power (buck input validated in the current design) and a USB-to-UART adapter if not using OTA uploads.
- Development host with Python 3, [PlatformIO Core](https://platformio.org/install/cli), and a working `git` toolchain. Required Python build dependencies are installed automatically by `scripts/install_build_deps.py` when PlatformIO runs.【F:software/modbus-to-mqtt/scripts/install_build_deps.py†L1-L32】

### Build and Flash
1. Clone the repository and enter the firmware workspace:
   ```bash
   git clone https://github.com/joachimda/modbus-to-x.git
   cd modbus-to-x/software/modbus-to-mqtt
   ```
2. Choose an environment in `platformio.ini`:
   - `prod-board-v0-1-alpha` targets the custom board with OTA uploads enabled by default.
3. Build the firmware (PlatformIO will fetch dependencies and embed git metadata):
   ```bash
   pio run
   ```
4. Flash via USB/UART or OTA depending on the selected environment:
   ```bash
   # Serial upload (comment upload_protocol, upload_port and upload_flags)
   pio run -e prod-board-v0-1-alpha -t upload

   # OTA upload to a provisioned gateway (uncomment upload_protocol, upload_port and upload_flags)
   pio run -e prod-board-v0-1-alpha -t upload
   ```
5. Upload the web UI and config assets to SPIFFS when they change:
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
It injects correct MIME types, disables caching, and emulates the `/api/stats/system`, `/api/logs`, and reboot endpoints used by the SPA.

## Usage

### First Boot and Network Setup
1. Power the gateway; it will advertise a Wi-Fi AP using the defaults defined in `Config.h` (`MODBUS-MQTT-BRIDGE` / `you-shall-not-pass`).
2. Connect to the AP. The ESP will try getting 192.168.4.1 from DHCP. If supported on your OS, you will be prompted to open the Captive Portal. If these fail, check your router for the device's IP address, and use your browser to open the configuration page `http://192.168.4.1/`. 
3. Follow the on-screen guidance to scan, join, or provision hidden networks, optionally specifying static IP parameters before saving.
4. After a successful network connection is established, reboot the device.

### Configuring Modbus Devices
- Use **Configure Modbus** to edit the RS-485 bus, add devices, and define datapoints. Modbus configurations are stored in SPIFFS `/conf/config.json` and can be applied live without rebooting.
- Configuration files follow the schema in `docs/configuration_examples/modbus.json` and the default example stored in SPIFFS (`data/conf/config.json`). Each datapoint specifies the function code, register address, data type, scale, and engineering units that will be published when polled.

### MQTT Publishing
- Configure broker host, port, credentials, and optional root topic via the **Configure MQTT** page or by editing `/conf/mqtt.json`. The firmware automatically extracts hostnames from URLs and persists passwords in NVS preferences.
- When Modbus reads succeed, datapoint values are published to MQTT using either the configured topic override or the default pattern `<root>/<device>/<datapointId>` with slugified datapoint names as datapoint IDs.
- MQTT connectivity, Modbus statistics, and recent logs are visible on the dashboard.

## Project Structure
```
.
├── hardware/                 # Schematics, PCB layout, BOM assets, release notes
├── software/
│   └── modbus-to-mqtt/       # PlatformIO workspace with firmware, web UI, and tooling
└── README.md                 # This high-level overview
```

### Hardware Documentation
Detailed hardware notes, KiCad sources, datasheets, and per-revision release notes are kept in [`hardware/`](hardware/). Start with [`hardware/releases/v0.1-alpha/release_notes.md`](hardware/releases/v0.1-alpha/release_notes.md) for the current board spin and consult the KiCad project in [`hardware/kicad_files/`](hardware/kicad_files/) for schematics, layout, and fabrication outputs.

### Software Documentation
The firmware entry point, reusable libraries, and static assets live under [`software/modbus-to-mqtt/`](software/modbus-to-mqtt/). Key references:
- [`src/`](software/modbus-to-mqtt/src/) – firmware modules for Modbus polling, MQTT connectivity, OTA, and the async web server.
- [`data/`](software/modbus-to-mqtt/data/) – SPIFFS config files and web UI bundle served by the device.
- [`docs/configuration_examples/`](software/modbus-to-mqtt/docs/configuration_examples/) – JSON schemas and examples for Modbus and MQTT configuration.
- [`boards/`](software/modbus-to-mqtt/boards/) – custom PlatformIO board definition for the production hardware (ESP32, 16 MB flash).


## Acknowledgements
- Built on Espressif's ESP32 platform and the open-source libraries listed in `platformio.ini` (ModbusMaster, PubSubClient, ArduinoJson, ESPAsyncWebServer).
- Web UI incorporates community design elements credited in `data/pages/configure.html`.
- Component footprints and 3D models sourced from vendor libraries included under `hardware/kicad_files/lib/` with accompanying license terms.
