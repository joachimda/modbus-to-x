# Repository Guidelines

These guidelines apply to anyone changing this repository, human or coding
agent. They describe where things live, how to build and test, and the
conventions pull requests are reviewed against. The [README](README.md) is the
user-facing overview; [HTTP_API.md](HTTP_API.md) and
[OTA_LIFECYCLE.md](OTA_LIFECYCLE.md) are the contracts the firmware and web UI
must keep honouring.

## Project Structure & Module Organization

The repository combines an ESP32 firmware project with its custom hardware
design.

Firmware lives in `software/modbus-to-mqtt/` (a PlatformIO workspace):

- `src/` holds the implementation, grouped into `logging/`, `modbus/`, `mqtt/`,
  `network/`, `services/`, `storage/` and `utils/`, with `main.cpp` as the
  entry point. Headers live under `include/` and mirror the `src/` paths;
  `include/concurrency/` and `include/constants/` are header-only.
- Platform-neutral logic is kept in `*Core` headers and sources that have no
  Arduino or FreeRTOS dependencies, so native tests can include them directly.
- `lib/esp-logger/` is the bundled logging library.
- `data/` is the SPIFFS image: `index.html`, `pages/*.html`, `style.css`,
  `js/` (ES modules; `js/pages/` holds per-page modules) and
  `conf/schema.json`.
- `test/sample_test.cpp` is the native Unity test suite, `test/stubs/` provides
  the minimal Arduino and ESP-IDF headers it needs, and `test/web/*.test.mjs`
  are the Node tests for the web UI modules.
- `scripts/` holds PlatformIO extra scripts (`install_build_deps.py`,
  `gen_version.py`), the mocked UI server (`run_test_webserver.py`) and
  owner-only hardware stress tools (`run_owner_stress.py`,
  `run_full_owner_hardware_stress.py`, `run_mqtt_test_broker.py`).
- `boards/mbx_custom_board.json` is the custom board definition and
  `mbx_partitions.csv` the partition table.
- `docs/configuration_examples/` has example Modbus and MQTT configuration
  files.

Hardware lives in `hardware/`: the KiCad project and its libraries in
`kicad_files/` (fabrication outputs under `kicad_files/manufactoring/`),
per-revision release notes under `releases/`, and datasheets under
`datasheets/`. Do not hand-edit generated Gerber, ZIP, STEP, PDF or binary
artifacts; regenerate them from the sources.

## Build, Test, and Development Commands

Run firmware commands from `software/modbus-to-mqtt/`:

- `pio run` builds the default development firmware (`mbx-custom-board-dev`).
- `pio run -e mbx-custom-board-dev -t buildfs` builds the SPIFFS image; run it
  after changing `data/`.
- `pio test -e native-test` runs the host-side Unity tests without ESP32
  hardware.
- `node --test test/web/*.test.mjs` runs the web UI tests (CI uses Node 24).
  One of them starts `scripts/run_test_webserver.py`, so `python3` must be on
  the path.
- `pio run -e mbx-custom-board-dev -t upload` flashes firmware over serial;
  `-t uploadfs` flashes the web and config assets.
- `python3 scripts/run_test_webserver.py` serves `data/` with mocked endpoints
  at `http://127.0.0.1:8000`. Use `--no-open` to keep it from launching a
  browser; see the README for the provisioning and OTA scenario flags.

The `mbx-custom-board-dev-ota` and `mbx-custom-board-release-primer`
environments upload over the network. They read the device password from the
`MBX_DEV_OTA_PASSWORD` environment variable and carry the owner's device
address in `upload_port`; change the address locally and leave that change out
of commits.

CI builds the firmware and SPIFFS image and runs the native and web test
suites on pushes and pull requests to `main`, but only when the change touches
`software/` (Markdown and `software/modbus-to-mqtt/docs/` excluded) or the CI
workflow and its composite action under `.github/`. Hardware-only and docs-only
changes get no run. Tagged releases are built and signed by the separate
Release OTA workflow.

## Coding Style & Naming Conventions

Follow nearby code: four-space indentation, braces on the declaration line,
and small, focused modules. Keep diffs narrow; no repository-wide formatter is
configured, so preserve the style of touched files.

C++: classes and namespaces use `PascalCase`, functions and variables use
`camelCase`, and macros and constants use `UPPER_SNAKE_CASE`. New headers use
`#ifndef` include guards named `MODBUS_TO_MQTT_<FILE>_H`, the prevailing
style. Header paths mirror `src/` paths. Logic that can be
tested natively belongs in a `*Core` unit with no Arduino includes.

JavaScript: ES modules, `camelCase` functions, `UPPER_SNAKE_CASE` for
module-level constants and frozen lookup objects (such as the `API` route map
in `data/js/app.js`). Logic shared between pages and tests lives in `.mjs`
modules that the page scripts import with relative paths; the HTML pages load
their entry scripts through an import map.

## Testing Guidelines

Native tests use PlatformIO's Unity framework. Name test functions
`test_<behavior>`, register each with `RUN_TEST`, and add them to
`test/sample_test.cpp`, which includes the `*Core` translation units it
exercises directly. If a unit needs an Arduino or ESP-IDF header that is not
yet stubbed, add a minimal stub under `test/stubs/` rather than pulling in the
framework.

Web tests use `node:test` and `node:assert/strict` and import the modules
under `data/js` directly. Keep DOM-free logic in `.mjs` modules so it stays
testable this way.

Hardware, networking and OTA changes cannot be covered natively. Describe the
manual verification you did (board revision, firmware version, what was
exercised, what was not) in the pull request. There is no stated coverage
threshold, but new behavior and regressions should be exercised.

## Commit & Pull Request Guidelines

Recent commits use short imperative or past-tense summaries such as
`Run native and web test suites in CI` or
`Support Wavin AHC 9000 (function codes 0x43/0x44)`. Keep commits focused and
avoid `wip` messages. Pull requests follow the template under `.github/`:
explain the change, list the build and test commands you ran and their
results, call out untested paths and required hardware or configuration, and
link related issues. Include screenshots for UI changes and exported
schematic or PCB views for meaningful hardware changes. Bug reports, feature
requests and Modbus device support requests have issue forms.

## Security & Configuration

Do not commit Wi-Fi, MQTT or OTA credentials, signing keys, or local IP
overrides. Treat release binaries and manifests as generated artifacts produced
by the tagged-release workflow. Strip credentials and device addresses from
logs and configuration excerpts before attaching them to issues or pull
requests.
