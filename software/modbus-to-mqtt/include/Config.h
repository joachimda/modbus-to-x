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
#define RS485_DROP_LEADING_ZERO 1
#endif

// When dropping leading zeros, wait up to this many microseconds for the
// first non-zero byte to arrive so we can substitute it as the first byte.
// In most cases, 500-2000 us is enough.
#ifndef RS485_FIRSTBYTE_WAIT_US
#define RS485_FIRSTBYTE_WAIT_US 2500
#endif

#ifndef LED_A_PIN
#define LED_A_PIN 26
#endif

#ifndef LED_B_PIN
#define LED_B_PIN 25
#endif
#ifndef LED_C_PIN
#define LED_C_PIN 33
#endif

#define RESET_BUTTON_PIN 13
#define RESET_HOLD_TIME_MS 3000

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

/****************************************************
 * EEPROM & PREFERENCE STORAGE
 ****************************************************/
#define EEPROM_WRITE_CYCLE_MS 5

#define MQTT_PREFS_NAMESPACE "mqtt"
#define MODBUS_PREFS_NAMESPACE "modbus"

#define MODBUS_MAX_REGISTERS 100

/****************************************************
 * MQTT
 ****************************************************/
#define MQTT_RECONNECT_INTERVAL_MS 5000

#ifndef MQTT_OTA_BROKER
#define MQTT_OTA_BROKER "localhost"
#endif

#ifndef MQTT_BUFFER_SIZE
#define MQTT_BUFFER_SIZE 4096
#endif

/****************************************************
 * OTA
 ****************************************************/
#ifndef OTA_HTTP_USER
#define OTA_HTTP_USER "admin"
#endif
#ifndef OTA_HTTP_PASS
#define OTA_HTTP_PASS "admin"
#endif

#ifndef DEV_OTA_ARDUINO_PASS
#define DEV_OTA_ARDUINO_PASS "admin"
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

/****************************************************
 * System
 ****************************************************/
#ifndef SERIAL_OUTPUT_BAUD
#define SERIAL_OUTPUT_BAUD 115200
#endif
#endif
