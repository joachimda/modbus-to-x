# Contributing to Modbus-to-X

Thank you for helping with this project. This file explains how a change gets
from an idea to `main`. The [README](README.md) is the user guide, and
[AGENTS.md](AGENTS.md) describes the repository layout, the build and test
commands and the coding conventions in detail; this file does not repeat them.

## Before you start

Open an issue at
<https://github.com/joachimda/modbus-to-x/issues/new/choose> and pick the
form that fits:

- **Bug report** for something that does not work. Include the firmware
  version, hardware revision and the steps that reproduce it, and strip
  credentials and device addresses from logs and configuration excerpts.
- **Modbus device support** before adding a device. The form asks for the
  register map, the bus settings and whether you can test on the real device,
  which decides how a change can be verified.
- **Feature request** before writing code for anything beyond a small fix, so
  the approach can be agreed and the work does not collide with something
  already in progress. Typos, obvious bugs and documentation corrections can
  go straight to a pull request.

Issues without a form are fine for questions; discussions are not enabled on
this repository. For a security problem, open an issue that says you have a
security report and describes the impact without exploit details, and the
maintainer will arrange a private channel.

## Setting up

Follow [Prerequisites](README.md#prerequisites) and
[Build and Flash](README.md#build-and-flash) in the README. The firmware
build, the native tests and the web tests all run without hardware, and the
mock server described under
[Local UI Development](README.md#local-ui-development) serves the web UI with
mocked endpoints. You only need a board for changes that touch the device
itself.

## Making a change

1. Fork the repository and create a branch from `main`. Prefixes such as
   `feat/`, `bug/`, `docs/` and `chore/` are common but not required.
2. Keep the branch to one topic. Unrelated fixes you find along the way go in
   their own pull request.
3. Follow the conventions in [AGENTS.md](AGENTS.md): match the style of the
   files you touch, keep natively testable logic free of Arduino includes, and
   add native or web tests for new behaviour.
4. Update the documentation in the same change when behaviour changes:
   `README.md`, [HTTP_API.md](HTTP_API.md),
   [OTA_LIFECYCLE.md](OTA_LIFECYCLE.md), and
   `software/modbus-to-mqtt/data/conf/schema.json` for configuration fields.
5. Write commit subjects as one short line; the release workflow turns each
   subject into a release-note entry. Put detail in the commit body.
6. Never commit Wi-Fi, MQTT or OTA credentials, signing keys, device addresses
   or build outputs. The OTA upload environments read the device address and
   password from `MBX_DEV_OTA_HOST` and `MBX_DEV_OTA_PASSWORD` in your shell.

## Before opening a pull request

Run these from `software/modbus-to-mqtt/` and note the results for the pull
request:

```bash
pio run -e mbx-custom-board-dev              # firmware builds
pio run -e mbx-custom-board-dev -t buildfs   # when data/ changed
pio test -e native-test                      # host-side unit tests
node --test test/web/*.test.mjs              # web UI module tests
```

Changes to Modbus polling, RS-485 handling, Wi-Fi, MQTT, OTA or the on-device
web UI also need a run on real hardware. Record the board revision, firmware
version, the Modbus device and bus settings, and what you exercised. Say
plainly what you did not test; a known gap is easier to review than a guess.

## Opening the pull request

- Target `main`. The pull request template loads automatically; fill in the
  sections that apply and delete the rest. Link the issue the change resolves.
- Include screenshots for web UI changes.
- CI builds the firmware and runs both test suites for changes under
  `software/`. On a first contribution the run waits for a maintainer to
  approve it. CI is advisory: a maintainer's approving review is what merges a
  pull request, and `main` does not accept direct pushes.
- Respond to review by pushing further commits to the same branch. The
  maintainer may ask for a squash before merging.

## Hardware changes

The board lives in `hardware/kicad_files/` as a KiCad project.

- Change the KiCad sources only. Do not hand-edit or regenerate Gerbers, ZIPs,
  STEP, PDF or other outputs in a design pull request. Say in the pull request
  if the change needs new fabrication outputs; the maintainer decides when to
  produce them.
- Keep the file format the current files use. Saving the project from a KiCad
  release that upgrades the format is a change of its own, not something to
  bundle with a design edit.
- Run ERC and DRC and report the result. Warnings accepted on purpose are
  noted in the release notes under `hardware/releases/`.
- Attach exported schematic and PCB views to the pull request for anything
  beyond a value change.
- Project symbols live in `lib/mbx.kicad_sym`. Third-party symbols,
  footprints and 3D models go under `lib/` with their licence file in
  `lib/licenses/`, as the existing ones do.
- Update the release notes under `hardware/releases/` when the change is part
  of a board revision.

## Licence

The project is released under the [MIT Licence](LICENSE). By contributing you
agree that your contribution is licensed the same way. Third-party hardware
assets keep their own licences under `hardware/kicad_files/lib/licenses/`.
