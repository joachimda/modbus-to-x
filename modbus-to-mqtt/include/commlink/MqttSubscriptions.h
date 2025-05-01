#ifndef MQTTSUBSCRIPTIONS_H
#define MQTTSUBSCRIPTIONS_H


static String MQTT_ROOT_TOPIC = "default_topic";

/****************************************************
 * SYSTEM TOPICS
 ****************************************************/
static String SUB_NETWORK_RESET = "/network/reset";
static String SUB_MODBUS_CONFIG = "/modbus/registers/config";
static String SUB_MODBUS_CONFIG_LIST = "/modbus/registers/list";
static String SUB_SYSTEM_ECHO = "/system/echo";
static String SUB_SYSTEM_LOG = "/system/log";

/****************************************************/


#endif //MQTTSUBSCRIPTIONS_H
