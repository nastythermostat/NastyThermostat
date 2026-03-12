#pragma once
#include <Arduino.h>

struct AppConfig {
  // WiFi (wordt door WiFiManager ingevuld)
  String wifiSsid;
  String wifiPass;

  // MQTT
  String mqttServer = MQTT_SERVER;
  uint16_t mqttPort = MQTT_PORT;

  // Domoticz IDX (0 = disable)
  int domoticzIdxSetpoint = 0;
  int domoticzIdxTemp     = 0;

  // Domoticz MQTT Gateway OUT mode: "index" or "flat"
  String domoticzOutMode = "index";

  // Behavior
  // If enabled, the device performs a homing cycle before applying a newly
  // received setpoint (MQTT cmd / Domoticz / API). This improves repeatability.
  bool homingAfterReceivingSetpoint = true;

  // Intervals (seconden)
  uint16_t mqttPublishIntervalSec = 30;

  // Device name
  String deviceName = "NastyThermostat";
};
