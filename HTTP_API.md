# HTTP API request contract

The device deliberately supports passwordless administration on its local network. Browser and API clients must still distinguish intentional state changes from read-only requests.

## State-changing requests

Every state-changing request must use the method listed below and include:

```http
X-MBX-Request: 1
```

The request's `Host` must be the active station IP address, setup-AP IP address, configured device hostname, or the hostname with `.local` appended. A numeric port may follow the host. Hostnames are matched case-insensitively.

The marker is fixed and non-secret; it is cross-site request protection, not authentication. The device does not enable CORS for the marker. A request with a missing or invalid marker or host receives HTTP 403 and `{"error":"forbidden_request_context"}` before its handler, body parser, or upload callback runs.

OTA interactions use a short-lived RAM session. `POST /api/system/ota/session`
is the only route that accepts `Authorization: Bearer <base64-password>` and
performs password verification. Subsequent protected requests use the returned
token in `X-OTA-Session`. Their request-context check runs first; after it
succeeds, a missing, invalid, expired, or revoked token returns HTTP 401.

| Mode | Method | Path | Operation |
| --- | --- | --- | --- |
| Station | `PUT` | `/api/config/modbus` | Replace Modbus configuration |
| Station | `PUT` | `/api/config/mqtt` | Replace MQTT configuration |
| Station | `POST` | `/api/config/mqtt/secret` | Replace MQTT password |
| Station | `POST` | `/api/mqtt/test` | Test the MQTT connection |
| Station | `POST` | `/api/modbus/execute` | Execute an ad hoc Modbus operation |
| Station | `POST` | `/api/modbus/state/disable` | Disable the Modbus bus |
| Station | `POST` | `/api/modbus/state/enable` | Enable the Modbus bus |
| Both | `POST` | `/api/system/reboot` | Schedule a device reboot |
| Setup AP | `POST` | `/api/wifi/connect` | Start a Wi-Fi connection attempt |
| Setup AP | `POST` | `/api/wifi/cancel` | Cancel the connection attempt |
| Setup AP | `POST` | `/api/wifi/ap_off` | Complete saved provisioning and schedule a reboot |
| Both | `POST` | `/api/wifi/reset` | Erase network settings and restart |
| Both | `POST` | `/api/system/ota/firmware` | Upload firmware |
| Both | `POST` | `/api/system/ota/fs` | Upload the UI filesystem |
| Both | `POST` | `/api/system/ota/session` | Create a RAM-only OTA session |
| Both | `POST` | `/api/system/ota/manual/start` | Quiesce and reserve a manual upload |
| Both | `POST` | `/api/system/ota/http/check` | Check for an HTTP OTA release |
| Both | `POST` | `/api/system/ota/http/notes` | Fetch release notes |
| Both | `POST` | `/api/system/ota/http/apply` | Apply the pending release |
| Both | `POST` | `/api/system/ota/http/settings` | Change HTTP OTA settings |
| Both | `PUT` | `/api/system/ota/password` | Set or change the OTA password |
| Both | `DELETE` | `/api/system/ota/password` | Clear the OTA password |
| Both | `POST` | `/api/system/factory-reset` | Erase all device settings |

Modbus and MQTT configuration replacements validate the fully resolved MQTT topic plan before writing either configuration file. A rejected command-topic wildcard, duplicate, or state/command collision returns HTTP 400 with `error`, `reason`, `topic`, and the affected device/datapoint identities. The persisted configuration and active bridge remain unchanged.

`PUT /api/config/modbus` also validates the document itself. A document that fails returns HTTP 400 with `error` = `invalid_modbus_configuration` and `reason` = `invalid_json`, `invalid_scale`, `invalid_register_count`, `invalid_wavin_address` or `invalid_baud_rate` (`bus.baud` must be an integer of at least 1200). `message` states the rule for every reason but `invalid_json`; `deviceIndex` and `datapointIndex` locate the datapoint for `invalid_scale`, `invalid_register_count` and `invalid_wavin_address` and are absent for the other reasons. The persisted configuration is unchanged.

### Ad hoc Modbus execution

`POST /api/modbus/execute` takes the query parameters `devId`, `dpId`, `func_code` (1-6, 16, or 67/68 for Wavin), `addr` (0-65535), `len` (1-65535), optionally `slave` (1-247) and, for writes, `value`. A missing or out-of-range parameter returns HTTP 400 without touching the bus. The saved datapoint is looked up as `dpId` within the device `devId` (an unknown `devId` falls back to the first datapoint with that id); its data type, scale, register slice and word order are used to interpret register values.

- Reads (FC1-FC4) return `result.raw` (the registers received, at most 32) and `result.value`: the ASCII text for `TEXT`, otherwise the engineering value decoded by the datapoint's data type and scale, formatted exactly like the MQTT publication (a JSON number such as `-2.00`; `nan` is returned as a string). A read shorter than the data type's width omits `value` and returns `result.valueError` = `insufficient_registers` together with `result.width`. Without datapoint metadata, `value` is the first raw register.
- FC5 takes `value` as `0`, `1`, `false` or `true` (case-insensitive, surrounding whitespace ignored), the MQTT coil grammar. Any other text returns HTTP 400 with `error` = `invalid_write_value` and `reason` = `invalid_syntax` without touching the bus.
- FC6 and FC16 take `value` as an engineering value with the same grammar as MQTT commands. It is converted with the saved datapoint's data type, scale and word order; FC16 writes the type's register width regardless of `len`. When `devId`/`dpId` name no saved datapoint (for example a datapoint renamed or added in the editor but not yet saved) the write returns HTTP 400 with `error` = `unknown_datapoint` and is not performed. A value that cannot be encoded returns HTTP 400 with `error` = `invalid_write_value` and `reason` (`empty`, `invalid_syntax`, `non_finite`, `invalid_scale`, `out_of_range` or `unsupported_data_type`) without touching the bus. A successful register write echoes the written registers in `result.raw` and, in `result.value`, the engineering value they encode, decoded as whole registers of the data type used for the write (a byte slice does not apply).
- Wavin functions 67 (0x43 read) and 68 (0x44 write) take `addr` as the register index (0-255) and optionally `category` and `page` (both 0-255); giving one without the other returns HTTP 400. Without them the saved datapoint's category and page are used, and when `devId`/`dpId` name no saved datapoint the command returns HTTP 400 with `error` = `unknown_datapoint` instead of addressing category 0, page 0. 67 reads up to `len` registers (1-22) and returns them like FC3; the controller may answer with fewer registers than requested, and `result.raw` then holds only those received. 68 writes one register with the FC6 value rules. An index above 255 returns `code` 2 (`IllegalDataAddress`) and a read length outside 1-22 returns `code` 3 (`IllegalDataValue`), both without touching the bus. When given, `category` and `page` are echoed in `request`.
- 68 can instead take `values`, a comma-separated list of 1-22 raw register words (decimal or `0x` hex, 0-65535), and writes them in one frame from `addr` on, without the datapoint's encoding. Use it for pages the controller only accepts whole, such as the clock (category 5, page 0, `addr=0`: year, month, day, day of week with 0 = Monday, hour, minute, second). The request returns HTTP 400 without touching the bus when `values` comes with any other function or together with `value`, when it is empty or an entry does not parse, or when the words would run past index 255 (so an index above 255 is HTTP 400 here, not `code` 2). `len` is the word count and `values` is echoed in `request`. `result.raw` echoes the written words; `result.value` decodes the leading word(s) as for a single write (the saved datapoint's data type, or the first word without one) and carries no meaning for a raw page.
- Every executed command answers with `ok`, `code`, `state`, `devId`, `dpId` and the echoed `request`; a bus error keeps HTTP 200 with `ok` false and the Modbus exception in `code`/`state`.

`GET /reset` is not supported. Network reset is available only through guarded `POST /api/wifi/reset`.

An admitted reboot request returns HTTP 202 and `{"ok":true,"rebooting":true}` before the device restarts. While that reboot is pending, another request returns HTTP 409 and `{"ok":false,"error":"reboot_pending"}` without changing the original deadline. If the reboot task cannot be scheduled, the request returns HTTP 503 and `{"ok":false,"error":"reboot_unavailable"}` without restarting.

Provisioning completion is accepted only while the current Wi-Fi attempt is connected with a station IP and its credentials plus DHCP/static selection have been persisted. An admitted `POST /api/wifi/ap_off` returns HTTP 202 and `{"ok":true,"rebooting":true,"ip":"<station-ip>"}`. A request before that state returns HTTP 409 and `{"ok":false,"error":"provisioning_not_ready"}`. Pending and unavailable reboot scheduling use the same 409 and 503 errors as the manual reboot route. Rejected requests leave the setup portal running.

## Read-only routes

These operations do not require the mutation marker:

| Mode | Method | Path | Operation |
| --- | --- | --- | --- |
| Station | `GET` | `/`, `/configure`, and static asset paths | Web interface |
| Station | `GET` | `/conf/config.json`, `/conf/mqtt.json` | Configuration reads |
| Station | `GET` | `/api/logs`, `/api/stats/system` | Logs and statistics |
| Station | `GET` | `/api/events` | Server-sent events |
| Station | `GET` | `/api/system/ready` | Verify that the station route table is active |
| Setup AP | `GET` | `/` and static asset paths | Captive-portal interface |
| Setup AP | `GET` | `/api/ssids`, `/api/wifi/status` | Wi-Fi scan and connection status |
| Both | `GET` | `/api/system/ota/http/settings` | HTTP OTA settings |
| Both | `GET` | `/api/system/ota/status` | Latest OTA lifecycle snapshot |
| Both | `GET` | `/api/system/ota/events` | OTA-only `ota-progress` event stream |
| Setup AP | Any | Captive-portal detection paths | Read-only redirect to the setup-AP address |

Unknown setup-AP paths also perform only the captive-portal redirect. Read-only routes never write configuration, control attached equipment, start an update, erase storage, or reboot the device.

`GET /api/wifi/status` retains its existing fields and includes `provisioningReady`. A temporary connection can be `connected` while `provisioningReady` is false. Persistence failures also remain not ready and include a stable `reason`.

The station-only readiness response is HTTP 200 JSON `{"ok":true,"mode":"station"}` with `Cache-Control: no-store` and `Access-Control-Allow-Origin: *`. This narrowly scoped CORS response lets the captive page distinguish the normal route table from redirects, captive HTML, and unrelated successful responses after reboot.

Some operating-system captive browsers keep background requests bound to the setup
AP even after the client joins the configured WLAN. In that case the initial
90-second check times out without claiming success. The captive page retains the
station address and offers a one-request readiness retry plus an **Open dashboard**
navigation fallback; direct navigation lets the browser establish a route on the
new network.

## Non-browser clients

Send mutations directly to an allowed IP address or hostname and include the marker explicitly. For example:

```bash
curl --fail-with-body \
  -X POST \
  -H 'X-MBX-Request: 1' \
  http://192.168.4.1/api/wifi/reset
```

Add the route's content type and body where required. For protected OTA work,
create one session with the password bearer header and then send its returned
token as `X-OTA-Session`. The password and session token must not be placed in a
URL or persistent storage.

During an admitted installation, another apply or manual preflight returns HTTP
409 and `{"error":"ota_in_progress"}`. Other normal mutations return HTTP 503
with that body before request-body processing or runtime-job scheduling. Status,
OTA-only SSE, and the admitted artifact transfer remain available.

Manual firmware and filesystem uploads require preflight. Post JSON containing
`component` (`firmware` or `filesystem`) and the exact byte `size` to
`/api/system/ota/manual/start`, wait for the returned operation to reach
`awaiting_upload`, then add `X-OTA-Operation: <operationId>` to the multipart
upload. See [OTA lifecycle and recovery](OTA_LIFECYCLE.md) for the complete
session, progress, admission, and recovery protocol.
