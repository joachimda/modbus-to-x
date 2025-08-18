#ifndef CONFIG_H
#define CONFIG_H

/****************************************************
 * WIRING
 ****************************************************/
#ifndef RS485_DE_PIN
#define RS485_DE_PIN 32
#endif

#ifndef RS485_RE_PIN
#define RS485_RE_PIN 33
#endif

#ifndef LED_A_PIN
#define LED_A_PIN 2
#endif

#ifndef LED_B_PIN
#define LED_B_PIN 14
#endif
#ifndef LED_C_PIN
#define LED_C_PIN 14
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
