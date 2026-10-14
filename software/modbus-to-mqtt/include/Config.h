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

// How long a Wavin (0x43/0x44) request waits for its reply. The controller
// answers within a few milliseconds; the margin covers a wall display that
// shares the bus as a second master.
#ifndef WAVIN_RESPONSE_TIMEOUT_MS
#define WAVIN_RESPONSE_TIMEOUT_MS 1000U
#endif

// Attempts per Wavin read when the reply times out or fails its CRC. Attempts
// x WAVIN_RESPONSE_TIMEOUT_MS stays within the two seconds the OTA quiescence
// allows for one Modbus transaction.
#ifndef WAVIN_READ_ATTEMPTS
#define WAVIN_READ_ATTEMPTS 2U
#endif

// Attempts per Wavin 0x44 write, same rule and bound as reads. A retry after a
// lost reply rewrites the same absolute values, so it cannot double-apply.
#ifndef WAVIN_WRITE_ATTEMPTS
#define WAVIN_WRITE_ATTEMPTS 2U
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
// plus local owner-loop margin.
#ifndef OTA_MQTT_QUIESCE_TIMEOUT_MS
#define OTA_MQTT_QUIESCE_TIMEOUT_MS 1500U
#endif
#ifndef OTA_MQTT_SIGNAL_SERVICE_WINDOW_MS
#define OTA_MQTT_SIGNAL_SERVICE_WINDOW_MS 100U
#endif
#ifndef OTA_MODBUS_QUIESCE_TIMEOUT_MS
#define OTA_MODBUS_QUIESCE_TIMEOUT_MS 2500U
#endif
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
