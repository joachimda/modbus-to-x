#ifndef CONFIG_H
#define CONFIG_H

/****************************************************
 * WIRING
 ****************************************************/
#ifndef RS485_DERE_PIN
#define RS485_DERE_PIN 15
#endif

// Guard delay around RS485 direction changes (in microseconds)
// Helps transceiver settle and ensures RX is enabled before responses arrive.
#ifndef RS485_DIR_GUARD_US
#define RS485_DIR_GUARD_US 1000
#endif

// Drop one or more leading 0x00 bytes seen immediately at the start of RX.
// Some RS485 transceivers or wiring transitions can produce a spurious 0x00
// which misaligns the Modbus frame. Enable to filter these.
#ifndef RS485_DROP_LEADING_ZERO
#define RS485_DROP_LEADING_ZERO 0
#endif

// When dropping leading zeros, wait up to this many microseconds for the
// first non-zero byte to arrive so we can substitute it as the first byte.
// In most cases, 500-2000 us is enough.
#ifndef RS485_FIRSTBYTE_WAIT_US
#define RS485_FIRSTBYTE_WAIT_US 2500
#endif

#ifndef WIFI_LED_A_PIN
#define WIFI_LED_A_PIN 25
#endif

#ifndef MQTT_LED_B_PIN
#define MQTT_LED_B_PIN 26
#endif
#ifndef MB_LED_C_PIN
#define MB_LED_C_PIN 27
#endif

/****************************************************
 * MODBUS
 ****************************************************/
#ifndef DEFAULT_MODBUS_BAUD_RATE
#define DEFAULT_MODBUS_BAUD_RATE 9600
#endif

#ifndef DEFAULT_MODBUS_MODE
#define DEFAULT_MODBUS_MODE "8N1"
#endif

#define MODBUS_SLAVE_ID 1

// Lowest baud rate the bus configuration form accepts. Wavin timing is
// checked against it at compile time (WavinClient.cpp).
#ifndef MODBUS_MIN_BAUD_RATE
#define MODBUS_MIN_BAUD_RATE 1200U
#endif

// How long a Wavin (0x43/0x44) attempt waits for its reply to start. The
// controller answers within a few milliseconds; a request lost to a
// collision with the wall display (a second master on the bus) gets no reply
// at all, so a short wait plus a retry recovers faster than a long one. The
// time the reply itself takes on the wire is added per attempt from the
// bus's baud rate and character size, so a full 22-register read also
// completes at low baud rates (about 13 ms at 38400 8N1, 409 ms at 1200).
#ifndef WAVIN_TURNAROUND_MS
#define WAVIN_TURNAROUND_MS 500U
#endif

// Attempts per Wavin read and 0x44 write when the reply times out or fails its
// CRC. 0x44 sets absolute values, so repeating a write whose reply was lost
// cannot double-apply. This is an upper bound: a retry starts only if it
// still fits WAVIN_TRANSACTION_BUDGET_MS.
#ifndef WAVIN_READ_ATTEMPTS
#define WAVIN_READ_ATTEMPTS 3U
#endif
#ifndef WAVIN_WRITE_ATTEMPTS
#define WAVIN_WRITE_ATTEMPTS 3U
#endif

// Random pause before each retry, so it does not land in the same burst of
// display traffic that swallowed the previous attempt.
#ifndef WAVIN_RETRY_BACKOFF_MIN_MS
#define WAVIN_RETRY_BACKOFF_MIN_MS 100U
#endif
#ifndef WAVIN_RETRY_BACKOFF_MAX_MS
#define WAVIN_RETRY_BACKOFF_MAX_MS 250U
#endif

// Longest a Wavin transaction, retries included, may take. The OTA
// quiescence waits OTA_MODBUS_QUIESCE_TIMEOUT_MS for the Modbus owner to
// finish the transaction in progress; that bound was sized for
// ModbusMaster's 2000 ms response timeout. A retry starts only if the pause
// before it plus a full attempt (request and reply on the wire, plus
// WAVIN_TURNAROUND_MS) still fit this budget. The 500 ms margin covers what
// is not counted: direction switching (RS485_DIR_GUARD_US), the input
// drain, the 1 ms polling granularity, the final frame scan, the quiet-line
// wait after a final failure and the other services' share of the main
// loop.
#ifndef WAVIN_TRANSACTION_BUDGET_MS
#define WAVIN_TRANSACTION_BUDGET_MS (OTA_MODBUS_QUIESCE_TIMEOUT_MS - 500U)
#endif

// After the last attempt has timed out or failed its CRC, input is discarded
// until the line has been quiet for WAVIN_SETTLE_QUIET_MS, measured from the
// last byte received and for at most WAVIN_SETTLE_MAX_MS. A reply that is
// still on its way, or the tail of the display burst that swallowed the
// attempt, is then not taken for the reply to the next request (Wavin
// replies carry no register address, so the reply itself cannot tell). The
// wait is part of the WAVIN_TRANSACTION_BUDGET_MS margin, which is checked
// at compile time where OTA_MODBUS_QUIESCE_TIMEOUT_MS is defined.
#ifndef WAVIN_SETTLE_QUIET_MS
#define WAVIN_SETTLE_QUIET_MS 50U
#endif
#ifndef WAVIN_SETTLE_MAX_MS
#define WAVIN_SETTLE_MAX_MS 150U
#endif

/****************************************************
 * PREFERENCE STORAGE
 ****************************************************/

#define MQTT_PREFS_NAMESPACE "mqtt"

/****************************************************
 * MQTT
 ****************************************************/
#define MQTT_RECONNECT_INTERVAL_MS 5000

#ifndef MQTT_BUFFER_SIZE
#define MQTT_BUFFER_SIZE 4096
#endif

#ifndef MQTT_COMMAND_QUEUE_DEPTH
#define MQTT_COMMAND_QUEUE_DEPTH 12
#endif

#ifndef MQTT_COMMAND_WAIT_MS
#define MQTT_COMMAND_WAIT_MS 3000
#endif

#ifndef MQTT_COMMANDS_PER_CYCLE
#define MQTT_COMMANDS_PER_CYCLE 6
#endif

#ifndef MODBUS_COMMAND_QUEUE_DEPTH
#define MODBUS_COMMAND_QUEUE_DEPTH 8
#endif

#ifndef MODBUS_COMMAND_WAIT_MS
#define MODBUS_COMMAND_WAIT_MS 5000
#endif

#ifndef MODBUS_COMMANDS_PER_CYCLE
#define MODBUS_COMMANDS_PER_CYCLE 8
#endif

#ifndef RUNTIME_HTTP_JOB_QUEUE_DEPTH
#define RUNTIME_HTTP_JOB_QUEUE_DEPTH 8
#endif

#ifndef RUNTIME_HTTP_WORKER_COUNT
#define RUNTIME_HTTP_WORKER_COUNT 3
#endif

// A job can be either queued or owned by one worker. Keeping one completion
// slot for every such job lets workers hand responses to the Arduino loop
// without waiting or touching AsyncTCP from their own task contexts.
#ifndef RUNTIME_HTTP_RESPONSE_QUEUE_DEPTH
#define RUNTIME_HTTP_RESPONSE_QUEUE_DEPTH (RUNTIME_HTTP_JOB_QUEUE_DEPTH + RUNTIME_HTTP_WORKER_COUNT)
#endif

/****************************************************
 * OTA
 ****************************************************/
#ifndef OTA_HTTP_ENABLED
#define OTA_HTTP_ENABLED 1
#endif
#ifndef OTA_HTTP_MANIFEST_URL
#define OTA_HTTP_MANIFEST_URL "https://github.com/joachimda/modbus-to-x/releases/latest/download/modbus-to-x.manifest.json"
#endif
#ifndef OTA_HTTP_RELEASES_API_URL
#define OTA_HTTP_RELEASES_API_URL "https://api.github.com/repos/joachimda/modbus-to-x/releases?per_page=1"
#endif
#ifndef OTA_HTTP_DEVICE
#define OTA_HTTP_DEVICE "modbus-to-x"
#endif
#ifndef OTA_HTTP_INTERVAL_MS
#define OTA_HTTP_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)
#endif
#ifndef OTA_HTTP_CA_CERT_PEM
#define OTA_HTTP_CA_CERT_PEM nullptr
#endif
#ifndef OTA_HTTP_FORCE_UPDATE_ON_NEW_VERSION
#define OTA_HTTP_FORCE_UPDATE_ON_NEW_VERSION 1
#endif

// OTA quiescence is bounded independently for each owner. ModbusMaster uses a
// two-second response timeout, so the Modbus allowance covers one transaction
// plus local owner-loop margin; a Wavin transaction, retries included, is
// held to WAVIN_TRANSACTION_BUDGET_MS for the same reason.
#ifndef OTA_MQTT_QUIESCE_TIMEOUT_MS
#define OTA_MQTT_QUIESCE_TIMEOUT_MS 1500U
#endif
#ifndef OTA_MQTT_SIGNAL_SERVICE_WINDOW_MS
#define OTA_MQTT_SIGNAL_SERVICE_WINDOW_MS 100U
#endif
#ifndef OTA_MODBUS_QUIESCE_TIMEOUT_MS
#define OTA_MODBUS_QUIESCE_TIMEOUT_MS 2500U
#endif
static_assert(WAVIN_TRANSACTION_BUDGET_MS < OTA_MODBUS_QUIESCE_TIMEOUT_MS,
              "the Wavin transaction budget must leave margin within the Modbus quiesce timeout");
// The quiet-line wait after a final failure runs after the budget and must
// fit in that margin, or a quiesce arriving during a failing transaction
// could miss the timeout.
static_assert(WAVIN_TRANSACTION_BUDGET_MS + WAVIN_SETTLE_MAX_MS < OTA_MODBUS_QUIESCE_TIMEOUT_MS,
              "the Wavin transaction budget plus the quiet-line wait must leave margin within the "
              "Modbus quiesce timeout");
#ifndef OTA_RUNTIME_DRAIN_TIMEOUT_MS
#define OTA_RUNTIME_DRAIN_TIMEOUT_MS 6000U
#endif
#ifndef OTA_RESUME_TIMEOUT_MS
#define OTA_RESUME_TIMEOUT_MS 2500U
#endif
#ifndef OTA_MANUAL_UPLOAD_TIMEOUT_MS
#define OTA_MANUAL_UPLOAD_TIMEOUT_MS 30000U
#endif

// Internal-memory admission floors are checked in two stages. The quiescence
// floors are checked once normal services are quiescent, before any TLS
// session or multipart upload has been set up. The flash-boundary floors are
// re-checked immediately before Update.begin, after TLS (signed path) or the
// multipart request (manual path) has already consumed memory. A TLS session
// costs roughly 45 KiB of internal heap on ESP32, so the two stages differ.
// A -D override must keep the flash-boundary floors at or below the
// quiescence floors; the static_asserts below enforce that ordering.
#ifndef OTA_MIN_INTERNAL_FREE_HEAP
#define OTA_MIN_INTERNAL_FREE_HEAP (96U * 1024U)
#endif
#ifndef OTA_MIN_INTERNAL_LARGEST_BLOCK
#define OTA_MIN_INTERNAL_LARGEST_BLOCK (32U * 1024U)
#endif
#ifndef OTA_FLASH_MIN_INTERNAL_FREE_HEAP
#define OTA_FLASH_MIN_INTERNAL_FREE_HEAP (48U * 1024U)
#endif
#ifndef OTA_FLASH_MIN_INTERNAL_LARGEST_BLOCK
#define OTA_FLASH_MIN_INTERNAL_LARGEST_BLOCK (16U * 1024U)
#endif
static_assert(OTA_FLASH_MIN_INTERNAL_FREE_HEAP <= OTA_MIN_INTERNAL_FREE_HEAP,
              "flash-boundary heap floor must not exceed the quiescence floor");
static_assert(OTA_FLASH_MIN_INTERNAL_LARGEST_BLOCK <= OTA_MIN_INTERNAL_LARGEST_BLOCK,
              "flash-boundary block floor must not exceed the quiescence floor");
#ifndef OTA_PROGRESS_MIN_INTERNAL_FREE_HEAP
#define OTA_PROGRESS_MIN_INTERNAL_FREE_HEAP (64U * 1024U)
#endif
#ifndef OTA_PROGRESS_MIN_INTERNAL_LARGEST_BLOCK
#define OTA_PROGRESS_MIN_INTERNAL_LARGEST_BLOCK (20U * 1024U)
#endif
#ifndef OTA_SESSION_ABSOLUTE_LIFETIME_MS
#define OTA_SESSION_ABSOLUTE_LIFETIME_MS (15U * 60U * 1000U)
#endif
#ifndef OTA_SESSION_IDLE_LIFETIME_MS
#define OTA_SESSION_IDLE_LIFETIME_MS (5U * 60U * 1000U)
#endif

#ifndef DEV_OTA_ARDUINO_PASS
#define DEV_OTA_ARDUINO_PASS ""
#endif
#ifndef DEV_OTA_ENABLED
#define DEV_OTA_ENABLED 0
#endif

/****************************************************
 * WIFI SETUP
 ****************************************************/
#ifndef DEFAULT_AP_SSID
#define DEFAULT_AP_SSID "MODBUS-MQTT-BRIDGE"
#endif

#ifndef DEFAULT_AP_PASS
#define DEFAULT_AP_PASS "you-shall-not-pass"
#endif

#ifndef DEFAULT_HOSTNAME
#define DEFAULT_HOSTNAME "modbus-to-x"
#endif

/****************************************************
 * System
 ****************************************************/
#ifndef SERIAL_OUTPUT_BAUD
#define SERIAL_OUTPUT_BAUD 115200
#endif
#endif
