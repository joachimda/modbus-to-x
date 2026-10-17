# OTA lifecycle and recovery

Signed and manual HTTP updates use one coordinator. The coordinator admits only
one installation and moves it through `quiescing`, a component download/write
stage, component verification, and `rebooting`. Manual uploads also use
`awaiting_upload`. A recoverable error moves through `failure_detected` and
`resuming` before retaining a terminal `failed` snapshot.

Checking for a release and fetching release notes do not quiesce the device.
Quiescence starts only after apply or manual-upload preflight is accepted, and
no `Update.begin` or partition write occurs until it succeeds.

## Quiescence order and bounds

The coordinator closes normal HTTP jobs, UI-filesystem leases, the normal
statistics/log event stream, MQTT admission, and Modbus admission first. MQTT
and Modbus owners then process priority quiesce commands concurrently.

| Guard | Bound | Behavior |
| --- | ---: | --- |
| MQTT owner acknowledgment | 1,500 ms | Publish the configured retained will/availability payload when connected, service the client for at most 100 ms, disconnect, and suppress reconnects |
| Modbus owner acknowledgment | 2,500 ms | Finish at most the RTU transaction already in progress (a Wavin transaction with its retries is held to 2,000 ms, plus a quiet-line wait of at most 150 ms after a final failure), then deactivate the bus |
| HTTP jobs, event clients, OTA check/notes transport, and UI-file leases | 6,000 ms | Drain admitted work and close the normal event stream |
| Failure resume acknowledgment | 2,500 ms | Restore matching MQTT and Modbus owner state |
| Manual upload start or chunk idle | 30,000 ms | Fail and resume if the declared upload never starts or the transfer stalls |

An MQTT maintenance signal is best effort. The device does not connect to a
broker solely to send it, and a disconnected broker or failed publish cannot
extend the bounded entry phase. The current bridge contract publishes its
configured will payload—normally retained `offline`—to its resolved will topic
before graceful disconnect. Queued and later ordinary MQTT work is rejected.
The setting that enabled or disabled MQTT and Modbus is not changed or
persisted during OTA.

Before flashing, the device logs internal free/minimum heap, largest allocatable
internal block, MQTT and Modbus queue depths, HTTP-job count, UI-file leases,
normal SSE clients, and MQTT signal result. Memory admission has two stages.
Quiescence admission requires at least 96 KiB free internal heap and a 32 KiB
largest internal block before any TLS session or multipart upload is set up.
Flash-boundary admission is re-checked immediately before `Update.begin` and
requires 48 KiB free internal heap and a 16 KiB largest block, because a TLS
session consumes roughly 45 KiB of internal heap on this platform. Both
refusals report `insufficient_heap`. OTA progress transport stops sending below
64 KiB free internal heap or a 20 KiB largest block, or while an SSE client
queue is non-empty; in the 48-64 KiB band the download continues while
progress updates pause.

## HTTP admission during an installation

Invalid mutation context still returns HTTP 403 first. Protected OTA requests
then validate `X-OTA-Session`. A second apply or manual preflight returns HTTP
409 with `{"error":"ota_in_progress"}`. Other normal mutations return HTTP
503 with the same error before their body handler or runtime job runs.

`GET /api/system/ota/status`, `GET /api/system/ota/events`, and the already
admitted manual artifact upload remain available. The normal `/api/events`
stream is closed, and reconnects receive HTTP 503 with
`{"error":"ota_in_progress"}` until failure recovery or reboot.
Statistics and logs are not mixed into the OTA-only stream.

## One-verification OTA sessions

Create a session with:

```http
POST /api/system/ota/session
X-MBX-Request: 1
Authorization: Bearer <base64-encoded OTA password>
```

The `Authorization` header is required only when OTA password protection is
enabled. A successful response contains a random 256-bit token encoded as 64
hexadecimal characters. Send that token on protected OTA interactions as:

```http
X-OTA-Session: <token>
```

The device holds at most two tokens in RAM. A token expires after 15 minutes
absolute or 5 minutes idle. Tokens and passwords are never placed in URLs,
logs, MQTT, SPIFFS, NVS, or browser persistent storage. Sessions are revoked on
password change/clear, factory reset, terminal OTA success or failure, and
reboot. Authorization is cached for an admitted upload so expiry cannot split a
single transfer.

Status and OTA progress are intentionally public read-only metadata, allowing a
native `EventSource` without a bearer secret in its URL.

## Manual upload protocol

Manual updates are two-step operations:

1. Create an OTA session.
2. `POST /api/system/ota/manual/start` with JSON such as
   `{"component":"firmware","size":1428045}`.
3. Observe status or OTA SSE until the matching operation reaches
   `awaiting_upload`.
4. Upload multipart data to `/api/system/ota/firmware` or
   `/api/system/ota/fs`, adding both `X-OTA-Session` and the decimal
   `X-OTA-Operation` returned by preflight.

Missing, stale, mismatched, or not-yet-ready operations return HTTP 409 before
partition work starts. A manual operation also fails and resumes normal services
if upload start or progress stalls for 30 seconds.

## Progress transport

`GET /api/system/ota/status` returns the latest fixed-size snapshot. The
OTA-only event stream at `/api/system/ota/events` sends the same JSON as an
`ota-progress` event, coalesced to the latest value and normally limited to
about one event per second. A representative snapshot is:

```json
{
  "operationId": 12,
  "sequence": 37,
  "targetVersion": "8.1.0",
  "state": "downloading_writing_firmware",
  "stage": "downloading_writing_firmware",
  "component": "firmware",
  "receivedBytes": 524288,
  "totalBytes": 1428045,
  "componentPercentKnown": true,
  "componentPercent": 36,
  "overallPercentKnown": false,
  "overallPercent": 0,
  "result": "none",
  "error": ""
}
```

Sequence numbers increase within an operation. Clients ignore duplicate,
out-of-order, and foreign-operation snapshots. Overall percentage is reported
only when all selected component sizes are known; no size is invented for a
skipped component. If SSE fails, the UIs poll the status endpoint in a
single-flight loop near every three seconds and periodically retry SSE.

## Filesystem and failure recovery

Every static/UI request owns a filesystem lease until disconnect. OTA closes
new lease admission and waits for existing leases before unmounting SPIFFS for
a filesystem write. During installation, `/` is served from a page compiled
into firmware; other static paths return the small in-progress response without
touching SPIFFS.

On a recoverable failure, active Update work is aborted, SPIFFS is remounted
without formatting, matching MQTT and Modbus owner states are restored, normal
HTTP/SSE admission reopens, and the terminal failure remains readable. If
SPIFFS cannot be remounted, only the compiled recovery page is enabled.

This reduces concurrent load but does not make the single UI filesystem
partition transactional. Power loss or an interrupted filesystem write may
still leave that partition unusable; the compiled page is the recovery path.
