#ifndef ARDUINO_SECRETS_H
#define ARDUINO_SECRETS_H

// MQTT instellingen
#define MQTT_SERVER "192.168.1.141"
#define MQTT_PORT   1883

// MQTT topics
// Standaard IN/OUT topics (handig voor hergebruik in andere projecten)
// Outgoing (status):  <base>/out/...
// Incoming (command): <base>/in/...
#define MQTT_TOPIC_BASE             "NastyThermostat"
#define MQTT_TOPIC_TEMP             MQTT_TOPIC_BASE "/out/temp"
#define MQTT_TOPIC_SETPOINT         MQTT_TOPIC_BASE "/out/setpoint"
#define MQTT_TOPIC_SETPOINT_CMD     MQTT_TOPIC_BASE "/in/setpoint"

// Domoticz MQTT Gateway input topic (voor uitgaande updates naar Domoticz)
#define MQTT_TOPIC_DOMOTICZ_IN      "domoticz/in"

// API settings
#define API_TOKEN "jouw_geheime_token"

#endif
