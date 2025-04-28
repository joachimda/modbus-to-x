#ifndef CONFIG_H
#define CONFIG_H

/****************************************************
 * WIRING
 ****************************************************/
#define RX_PIN 3
#define TX_PIN 1

#define RS485_DE_PIN 32
#define RS485_RE_PIN 33

#define RGB_LED_RED_PIN 2
#define RGB_LED_GREEN_PIN 16
#define RGB_LED_BLUE_PIN 4
#define RESET_BUTTON_PIN 13
#define RESET_HOLD_TIME_MS 3000

/****************************************************
 * MODBUS
 ****************************************************/
#define MODBUS_BAUD_RATE 9600
#define MODBUS_CONF_8E1 0x800001e
#define MODBUS_CONF_8N1 = 0x800001c
#define MODBUS_SLAVE_ID 1

/****************************************************
 * EEPROM & PREFERENCE STORAGE
 ****************************************************/
#define MODBUS_CONFIG_COUNT_ADDR 0x0020
#define MODBUS_CONFIG_ADDR_START 0x0022
#define EEPROM_WRITE_CYCLE_MS 5
#define MQTT_PREFS_NAMESPACE "mqtt"

/****************************************************
 * MQTT
 ****************************************************/
static constexpr auto MQTT_CLIENT_PREFIX = "MODBUS_CLIENT-";
static constexpr auto MQTT_RECONNECT_INTERVAL_MS = 5000;
static constexpr auto DEFAULT_MQTT_BROKER_PORT = "1883";
static constexpr auto DEFAULT_MQTT_BROKER_IP = "0.0.0.0";

/****************************************************
 * WIFI SETUP
 ****************************************************/
static constexpr auto DEFAULT_AP_SSID = "MODBUS-MQTT-BRIDGE";
static constexpr auto DEFAULT_AP_PASS = "you-shall-not-pass";
static constexpr auto WIFI_CONNECT_TIMEOUT = 10000;

/****************************************************
 * System
 ****************************************************/
static constexpr auto MQTT_TASK_STACK = 4096;
static constexpr auto MQTT_TASK_LOOP_DELAY_MS = 100;

#endif
