<!--
Thanks for contributing to Modbus-to-X. Fill in the sections that apply and
delete the ones that do not. All commands below run from software/modbus-to-mqtt/.
-->

## Summary

<!-- What changes and why. Link the issue it resolves, for example "Closes #12". -->

## Changes

<!-- Group by area: firmware, configuration/schema, web UI, MQTT/Home Assistant, HTTP API, hardware, docs. -->

-

## Testing

<!-- Tick what you ran and fill in the numbers. Say explicitly what was NOT tested. -->

- [ ] `pio run -e mbx-custom-board-dev` builds
- [ ] `pio run -e mbx-custom-board-dev -t buildfs` builds (required when `data/` changed)
- [ ] `pio test -e native-test` (N/N pass)
- [ ] `node --test test/web/*.test.mjs` (N/N pass)
- [ ] Web UI clicked through in a browser, on the device or via `python3 scripts/run_test_webserver.py`
- [ ] Hardware: board revision, Modbus device and bus settings (baud, parity, slave ID), serial or OTA flash, and what was exercised

Not tested:

## Documentation

- [ ] README, `HTTP_API.md` or `OTA_LIFECYCLE.md` updated where behaviour changed
- [ ] `data/conf/schema.json` updated when configuration fields changed
- [ ] Screenshots attached for UI changes

## Hardware changes

<!-- Delete this section if the PR does not touch hardware/. -->

- [ ] Only KiCad sources edited; no hand-edited Gerber, ZIP, STEP, PDF or binary outputs
- [ ] Exported schematic and PCB views attached
- [ ] Release notes under `hardware/releases/` updated

## Checklist

- [ ] No Wi-Fi, MQTT or OTA credentials, signing keys or local IP overrides in the diff
- [ ] Commits have focused, descriptive summaries (no `wip`)

## Notes

<!-- Follow-ups, open questions, anything the reviewer should look at first. -->
