/*
  === Nest Stepper + TFT Menu (ESP32-C6, ST7789) ================================
  v1.0.0 - First public release

  Changes in v1.0.0:
  - First public release on GitHub.
  - WiFi credentials removed from arduino_secrets.h.
  - homingNest: added 15-second safety timeout to moveStepperTo() so the motor
    stops automatically if the mechanical end-stop is missed.
  - handleApiRequest: fixed mixed tabs/spaces indentation in POST token-check block.

  v0.3.13 (pre-release)

  v0.3.12

  Changes in v0.3.12:
  - Device naming is now unique by default: "NastyThermostat" + last 6 chars of MAC.
  - Device name is configurable from the AP/WiFiManager settings page.
  - MQTT base topics are now derived from the configured device name, so multiple
    thermostats no longer collide on the same topics.
  - Home Assistant discovery labels/device metadata now follow the configured
    device name for clear per-device identification.
  - AP settings page now always shows all custom parameters, also on first setup.

  Changes in v0.3.11:
  - HTTP API token: use the token saved from AP settings (Preferences) for /api, instead of the build-time API_TOKEN.
    * If no token is configured: GET /api is allowed, POST /api is disabled.
    * If a token is configured: token is required for both GET (query ?token=) and POST (JSON field token).
  - Stepper motion: reduce how often we yield to the WiFi stack during long blocking moves to prevent micro-pauses.

  - Stepper motion smoothness: removed full-screen TFT redraws during blocking
    motor moves (homing / moving to temperature). Display is woken but the heavy
    redraw is deferred, preventing stutter.
  - Home Assistant: added retained "action" topic (heating/idle) and publish it
    on temperature and setpoint updates.
  - Home Assistant setpoint stability: target setpoint state is published as
    retained when HA discovery is enabled, preventing HA from jumping back to an
    older retained value.

  (v0.3.8 changes)
  - Home Assistant MQTT Discovery (optional): when MQTT is enabled, the device can
    publish a retained MQTT Discovery config so Home Assistant automatically adds a
    climate entity (setpoint + current temperature). Toggle via AP settings:
    "Home Assistant discovery (on/off)".
  - Nest wake behavior when "Homing after receiving setpoint" is OFF:
      - changed the wake-jiggle to 0.5°C (down & back)
      - uses the fast homing speed profile for this short jiggle
    This makes the movement feel more natural while still waking the Nest.
*/

#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

#include <AccelStepper.h>
#include <OneWireNg_CurrentPlatform.h>
#include <Preferences.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>

#include <WiFiManager.h>

#include "arduino_secrets.h"

// ===================== FIRMWARE INFO ========================================
#define FW_NAME    "NastyThermostat"
#define FW_VERSION "1.0.0"
#define HA_FW_NAME "Nasty"
#define HA_CLIMATE_NAME "Thermostat"

// Requested AP name
#define AP_SSID_PRIMARY  "NastyThermostat"
#define AP_SSID_FALLBACK "NastyESP32"

// Preference key to reboot into portal mode
#define CFG_NAMESPACE "config"
#define KEY_PORTAL_REQUEST "portalReq"
#define KEY_DZ_OUT_MODE   "dzOutMode"
#define KEY_HA_DISC       "haDisc"
#define KEY_HOME_AFTER_RX "homeAfterRx"
#define KEY_DEVICE_NAME   "devName"

// ===================== TFT PINNING (ESP32-C6 LCD 1.47) ======================
#define TFT_CS    14
#define TFT_DC    15
#define TFT_RST   21
#define TFT_BL    22
#define TFT_MISO  -1
#define TFT_MOSI  6
#define TFT_SCLK  7

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ===================== WIFI/MQTT/API ========================================
WiFiClient espClient;
PubSubClient client(espClient);
WebServer server(80);

WiFiManager wm;
volatile bool shouldSaveConfig = false;

// MQTT runtime enable (disabled if mqttServer is empty)
bool mqttEnabled = true;

// ===================== STEPPER / ENCODER / DS18B20 ==========================
#define dirPin 3
#define stepPin 2
#define enablePin 5
#define motorInterfaceType 1

#define encoderPinA 0
#define encoderPinB 1
#define encoderButtonPin 4

#define ONE_WIRE_PIN 9
#define ENABLE_PULLUP true

AccelStepper stepper(motorInterfaceType, stepPin, dirPin);
OneWireNg_CurrentPlatform ow(ONE_WIRE_PIN, ENABLE_PULLUP);

// Preferences
Preferences prefs;     // setpoint namespace
Preferences prefsCfg;  // config namespace

// ===================== RUNTIME CONFIG =======================================
struct AppConfig {
  String deviceName = "";
  String topicBase  = MQTT_TOPIC_BASE;

  // MQTT
  String mqttServer = MQTT_SERVER;
  uint16_t mqttPort = MQTT_PORT;


// HTTP API
// If empty: GET /api allowed, POST /api disabled (no writes).
// If set: token required for GET and POST.
String apiToken = API_TOKEN;


  // Topics
  String topicTemp        = MQTT_TOPIC_TEMP;
  String topicSetpoint    = MQTT_TOPIC_SETPOINT;
  String topicSetpointCmd = MQTT_TOPIC_SETPOINT_CMD;
  String topicSetpointIn  = "domoticz/out"; // computed from domoticzOutMode + idx
  String topicDomoticzIn  = MQTT_TOPIC_DOMOTICZ_IN;

  // Domoticz MQTT Gateway OUT mode: "index" or "flat"
  String domoticzOutMode  = "index";

  // Domoticz IDX
  int domoticzIdxSetpoint = 0;      // 0 = disabled
  int domoticzIdxTemp     = 0;      // 0 = disabled

  // Behavior
  bool homingAfterReceivingSetpoint = true;

  // Home Assistant (MQTT Discovery)
  bool homeAssistantDiscovery = true;

  // Interval
  uint16_t mqttPublishIntervalSec = 30;
};

AppConfig cfg;

// ===================== VARIABELEN ===========================================
volatile int  encoderDirection      = 0;
volatile bool encoderMovedMenu      = false;
volatile bool encoderMovedSetpoint  = false;
volatile bool buttonPressed         = false;

float Setpoint = 18.5;
float stepSize = 0.1;
float lastMeasuredTemp = 0.0;
bool hasTempReading = false;   // becomes true after first successful DS18B20 read
unsigned long lastActionPublishMs = 0; // periodic refresh for HA action topic


bool menuActive    = false;
bool subMenuActive = false;
int  menuIndex     = 0;

// submenu state
float offsetMenuValue = 0.0;
float calibrationStepsValue = 0.0;
int   stepSizeIndex = 0;

float temperatureOffset = 0.0;

unsigned long lastTempRequest = 0;
const unsigned long conversionTime = 750;
bool tempRequested = false;

unsigned long lastSetpointChange = 0;
const unsigned long setpointSaveDelay = 4000; // ms

// Stepper idle
unsigned long lastStepperMovement = 0;
const unsigned long stepperTimeout = 4000; // ms
bool stepperEnabled = true;

static inline void enableStepperDriver() {
  digitalWrite(enablePin, LOW); // DRV8825 enable is LOW
  stepperEnabled = true;
}

static inline void disableStepperDriver() {
  digitalWrite(enablePin, HIGH); // disable
  stepperEnabled = false;
}

// Overbrenging & microstepping
const int   stepperPulleyTeeth      = 20;
const int   nestPulleyTeeth         = 138;
const float microstepFactor          = 0.03125;    // 1/32
const int   stepsPerRevolutionMotor = 200;

// --- speed profiles ---
const float speedNormal  = 40.0;
const float accelNormal  = 20.0;
const float speedHoming  = 400.0;
const float accelHoming  = 300.0;
const float speedRotary  = 120.0;
const float accelRotary  = 80.0;

// MQTT publish interval (runtime)
unsigned long lastMqttPublish = 0;
unsigned long mqttInterval = 30000;

// publish tracking
float lastPublishedSetpoint = -999;
float lastPublishedTemp     = -999;

// --- loop/echo guards ---
// Some MQTT bridges (notably the Domoticz MQTT gateway) will echo a value we just
// published back on their OUT topic. If we treat that echo as a new command we can
// end up re-processing the same setpoint (and possibly re-homing / re-moving).
//
// Strategy:
// - When we publish a setpoint to Domoticz (domoticz/in), remember value + timestamp.
// - When a Domoticz OUT message arrives, ignore it if it matches the last value we
//   sent within a short guard window.
//
// This keeps things snappy while still allowing real external changes to flow through.
const unsigned long SETPOINT_ECHO_GUARD_MS = 1200;
float lastSentDomoticzSetpoint = -999;
unsigned long lastSentDomoticzSetpointMs = 0;
unsigned long lastSetpointStatePublishMs = 0;
bool  publishSetpointOnBootPending = true;

// Home Assistant "action" (heating/idle) publishing
// We keep it retained so HA doesn't bounce on reconnect.
String lastPublishedAction = "";

// Derived values
long  stepsPerMotorRevolution = (long)(stepsPerRevolutionMotor / microstepFactor);
float gearRatio             = (float)nestPulleyTeeth / (float)stepperPulleyTeeth;
long  stepsPerNestRevolution  = (long)(stepsPerMotorRevolution * gearRatio);

// Nest operating range
const float minNestTempC = 9.0;
const float maxNestTempC = 32.0;

// Encoder settle
const unsigned long ENCODER_SETTLE_MS = 1000;
bool pendingSetpointMove = false;

// External setpoint apply (MQTT/API/Domoticz) - optional "wake" jiggle.
// When homing-after-receive is OFF, we can briefly move 1°C down and back to
// "wake" the Nest before moving to the new setpoint.
enum ExternalMoveState {
  EXT_NONE = 0,
  EXT_WAKE_DOWN,
  EXT_WAKE_BACK,
  EXT_MOVE_NEW
};
ExternalMoveState extMoveState = EXT_NONE;
long extTargetDown = 0;
long extTargetBack = 0;
long extTargetNew  = 0;

// Remember where we came from for external moves
float extOldSetpoint = 0.0f;
float extNewSetpoint = 0.0f;

// steps/°C opslag (full-step basis)
float stepsPerDegreeBase;  // full-step opslag
float stepsPerDegreeCalc;   // omgerekend naar microsteps
float stepsPerDegreeNest;   // effectief gebruikt

// ===================== UI / DISPLAY STATE ===================================
unsigned long lastInteractionMs = 0;
bool screenOn = true;
const unsigned long SCREEN_TIMEOUT_MS = 15000;

// Allow stepper code to keep the screen awake.
// We disable this during boot-homing and during WiFiManager portal/AP mode,
// so the display stays off until the device is ready.
bool screenWakeAllowed = true;

// redraw throttle
float  lastShownSetpoint = -1000.0;
float  lastShownTemp     = -1000.0;
bool   lastWifiConnected = false;

// ===================== MENU ITEMS ===========================================
const int menuItemCount = 7;
const char* menuItems[menuItemCount] = {
  "1. Homing",
  "2. Temperature offset",
  "3. Steps/degree",
  "4. Step size",
  "5. WiFi setup",
  "6. Status",
  "7. Back"
};
const int menuStartY = 15;
const int menuLineHeight = 24;
const int menuIndentX = 10;

// Stapgroottes
const int STEP_SIZE_COUNT = 4;
const float stepSizeOptions[STEP_SIZE_COUNT] = {0.1, 0.2, 0.5, 1.0};

// WiFi submenu
int wifiSubIndex = 0;
const int wifiSubCount = 2;
const char* wifiSubItems[wifiSubCount] = {
  "Start WiFi portal (reboot)",
  "Back"
};


// ===================== PROTOTYPES ===========================================
void IRAM_ATTR handleButton();

void requestTemperature();
float readTemperature();
void primeTemperatureReading();


void saveSetpoint();

void homingNest(bool goToSetpoint = true);
void setNestToTemperature(float targetTemp);
void moveStepperTo(long absolutePosition, unsigned long timeoutMs = 0);

void setStepperProfileNormal();
void setStepperProfileRotary();
void setStepperProfileHoming();

void reconnectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void setupMQTT();
void mqttLoop();
void publishTemperature();
void publishSetpoint(bool force);

void publishDomoticzTemp();
void publishDomoticzSetpoint();

// Home Assistant discovery (MQTT)
void publishAvailability(bool online);
void publishHomeAssistantDiscovery();
void publishHomeAssistantModeHeat();
void publishHomeAssistantAction();

// Home Assistant topic helpers (forward declarations)
static String haTopicAvailability();
static String haTopicModeState();
static String haTopicModeCommand();
static String haTopicAction();

void checkEncoder();
void handleMenu();
void drawInfo();
void drawMenu();
void drawWifiIcon(bool connected);

void drawSubmenuOffset();
void drawSubmenuSteps();
void drawSubmenuStepSize();
void drawSubmenuWiFi();
void drawSubmenuStatus();

void wakeScreen();

void applyExternalSetpoint(float newSetpoint, const char* sourceTag);
void processExternalMoveState();

void handleApiRequest();
void handleSerialCommands();

void loadConfig();
void saveConfig();
bool runWiFiManagerPortal(bool forcedPortal, uint16_t portalTimeoutSec);
bool setupWiFiAuto();
void printBanner();

void requestPortalAndReboot();
bool consumePortalRequest();
static String defaultDeviceName();
static void rebuildDerivedTopics();
static String portalApName();
static String haPrettyDeviceName();

// ===================== WiFiManager callbacks ================================
void onSaveConfig() { shouldSaveConfig = true; }

static String macHex12() {
  char macBuf[13];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(macBuf, sizeof(macBuf), "%012llX", (unsigned long long)(mac & 0xFFFFFFFFFFFFULL));
  return String(macBuf);
}

static String macSuffix6() {
  String mac = macHex12();
  if (mac.length() < 6) return "000000";
  return mac.substring(mac.length() - 6);
}

static String defaultDeviceName() {
  return String(FW_NAME) + macSuffix6();
}

static String normalizeDeviceName(const String& raw) {
  String s = raw;
  s.trim();
  if (s.length() == 0) s = defaultDeviceName();

  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if ((uint8_t)c >= 32 && (uint8_t)c <= 126) out += c;
  }
  out.trim();
  if (out.length() == 0) out = defaultDeviceName();

  // Keep safe length for AP SSID and MQTT client id.
  if (out.length() > 31) out = out.substring(0, 31);
  return out;
}

static String topicSafeBaseFromName(const String& name) {
  String in = normalizeDeviceName(name);
  String out;
  out.reserve(in.length());

  for (size_t i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    if (isalnum((unsigned char)c) || c == '-' || c == '_') out += c;
    else out += '_';
  }

  while (out.indexOf("__") >= 0) out.replace("__", "_");
  while (out.startsWith("_")) out.remove(0, 1);
  while (out.endsWith("_")) out.remove(out.length() - 1);
  if (out.length() == 0) out = String(FW_NAME) + macSuffix6();
  return out;
}

static void rebuildDerivedTopics() {
  cfg.deviceName = normalizeDeviceName(cfg.deviceName);
  cfg.topicBase = topicSafeBaseFromName(cfg.deviceName);
  cfg.topicTemp = cfg.topicBase + "/out/temp";
  cfg.topicSetpoint = cfg.topicBase + "/out/setpoint";
  cfg.topicSetpointCmd = cfg.topicBase + "/in/setpoint";
}

static String portalApName() {
  String ssid = normalizeDeviceName(cfg.deviceName);
  if (ssid.length() == 0) ssid = AP_SSID_PRIMARY;
  if (ssid.length() > 31) ssid = ssid.substring(0, 31);
  return ssid;
}

static String haPrettyDeviceName() {
  String name = cfg.deviceName;
  const size_t fwLen = strlen(FW_NAME);

  // Make default auto-name more readable in HA: "NastyThermostat ABC123".
  if (name.startsWith(FW_NAME) && name.length() == (int)(fwLen + 6)) {
    name = String(FW_NAME) + " " + name.substring(fwLen);
  }

  return name + " v" + String(FW_VERSION);
}

// ===================== CONFIG LOAD/SAVE =====================================
void loadConfig() {
  prefsCfg.begin(CFG_NAMESPACE, true);

  cfg.deviceName = prefsCfg.getString(KEY_DEVICE_NAME, cfg.deviceName);
  cfg.mqttServer = prefsCfg.getString("mqttHost", cfg.mqttServer);
  cfg.mqttPort   = (uint16_t)prefsCfg.getUShort("mqttPort", cfg.mqttPort);
  cfg.apiToken   = prefsCfg.getString("apiToken", cfg.apiToken);

  cfg.domoticzIdxTemp     = prefsCfg.getInt("dzTemp", cfg.domoticzIdxTemp);
  cfg.domoticzIdxSetpoint = prefsCfg.getInt("dzSet", cfg.domoticzIdxSetpoint);

  cfg.domoticzOutMode = prefsCfg.getString(KEY_DZ_OUT_MODE, cfg.domoticzOutMode);
  cfg.homeAssistantDiscovery = prefsCfg.getBool(KEY_HA_DISC, cfg.homeAssistantDiscovery);

  cfg.homingAfterReceivingSetpoint = prefsCfg.getBool(KEY_HOME_AFTER_RX, cfg.homingAfterReceivingSetpoint);

  cfg.mqttPublishIntervalSec = (uint16_t)prefsCfg.getUShort("interval", cfg.mqttPublishIntervalSec);

  prefsCfg.end();

  // If mqttServer is empty -> MQTT disabled
  cfg.apiToken.trim();

  cfg.mqttServer.trim();
  mqttEnabled = (cfg.mqttServer.length() > 0);

  // Sanitize Domoticz OUT mode
  cfg.domoticzOutMode.trim();
  cfg.domoticzOutMode.toLowerCase();
  if (cfg.domoticzOutMode != "flat" && cfg.domoticzOutMode != "index") {
    cfg.domoticzOutMode = "index";
  }

  rebuildDerivedTopics();

  mqttInterval = (unsigned long)cfg.mqttPublishIntervalSec * 1000UL;
  if (mqttInterval < 1000UL) mqttInterval = 1000UL;
}

void saveConfig() {
  rebuildDerivedTopics();
  prefsCfg.begin(CFG_NAMESPACE, false);

  prefsCfg.putString(KEY_DEVICE_NAME, cfg.deviceName);
  cfg.mqttServer.trim();
  prefsCfg.putString("mqttHost", cfg.mqttServer);
  prefsCfg.putUShort("mqttPort", cfg.mqttPort);
  prefsCfg.putString("apiToken", cfg.apiToken);

  prefsCfg.putInt("dzTemp", cfg.domoticzIdxTemp);
  prefsCfg.putInt("dzSet", cfg.domoticzIdxSetpoint);

  prefsCfg.putString(KEY_DZ_OUT_MODE, cfg.domoticzOutMode);
  prefsCfg.putBool(KEY_HA_DISC, cfg.homeAssistantDiscovery);

  prefsCfg.putBool(KEY_HOME_AFTER_RX, cfg.homingAfterReceivingSetpoint);

  prefsCfg.putUShort("interval", cfg.mqttPublishIntervalSec);

  prefsCfg.end();

  mqttEnabled = (cfg.mqttServer.length() > 0);

  // Sanitize Domoticz OUT mode
  cfg.domoticzOutMode.trim();
  cfg.domoticzOutMode.toLowerCase();
  if (cfg.domoticzOutMode != "flat" && cfg.domoticzOutMode != "index") {
    cfg.domoticzOutMode = "index";
  }

  mqttInterval = (unsigned long)cfg.mqttPublishIntervalSec * 1000UL;
  if (mqttInterval < 1000UL) mqttInterval = 1000UL;

  // Important: reset timing so the new interval takes effect immediately
  lastMqttPublish = millis();
  Serial.print("✅ Config saved. Interval now = ");
  Serial.print(cfg.mqttPublishIntervalSec);
  Serial.println(" sec");
}

// ===================== Portal request flag ==================================
void requestPortalAndReboot() {
  prefsCfg.begin(CFG_NAMESPACE, false);
  prefsCfg.putBool(KEY_PORTAL_REQUEST, true);
  prefsCfg.end();

  Serial.println("🔁 Portal requested -> rebooting now...");
  delay(300);
  ESP.restart();
}

bool consumePortalRequest() {
  prefsCfg.begin(CFG_NAMESPACE, false);
  bool req = prefsCfg.getBool(KEY_PORTAL_REQUEST, false);
  if (req) prefsCfg.putBool(KEY_PORTAL_REQUEST, false);
  prefsCfg.end();
  return req;
}
// ===================== WIFI (WiFiManager) ===================================

// Helper: copy portal params -> cfg -> save
static void applyPortalParamsAndSave(
  WiFiManagerParameter& p_deviceName,
  WiFiManagerParameter& p_mqttServer,
  WiFiManagerParameter& p_mqttPort,
  WiFiManagerParameter& p_domTemp,
  WiFiManagerParameter& p_domSet,
  WiFiManagerParameter& p_dzMode,
  WiFiManagerParameter& p_homeAfterRx,
  WiFiManagerParameter& p_homeAssistant,
  WiFiManagerParameter& p_apiToken,
  WiFiManagerParameter& p_interval
) {
  cfg.deviceName = p_deviceName.getValue();
  cfg.deviceName.trim();
  if (cfg.deviceName.length() == 0) cfg.deviceName = defaultDeviceName();

  cfg.mqttServer = p_mqttServer.getValue();
  cfg.mqttServer.trim();
  cfg.mqttPort   = (uint16_t)atoi(p_mqttPort.getValue());
  cfg.domoticzIdxTemp     = atoi(p_domTemp.getValue());
  cfg.domoticzIdxSetpoint = atoi(p_domSet.getValue());

  {
    String m = String(p_dzMode.getValue());
    m.trim();
    m.toLowerCase();
    if (m.length() > 0) cfg.domoticzOutMode = m;
    cfg.domoticzOutMode.trim();
    cfg.domoticzOutMode.toLowerCase();
    if (cfg.domoticzOutMode != "flat" && cfg.domoticzOutMode != "index") cfg.domoticzOutMode = "index";
  }

  {
    String v = String(p_homeAfterRx.getValue());
    v.trim();
    v.toLowerCase();
    // Accept: on/off, true/false, 1/0, yes/no.
    // If the field is left empty, keep the current setting.
    if (v.length() > 0) {
      bool on = (v == "on" || v == "true" || v == "1" || v == "yes");
      bool off = (v == "off" || v == "false" || v == "0" || v == "no");
      if (on) cfg.homingAfterReceivingSetpoint = true;
      else if (off) cfg.homingAfterReceivingSetpoint = false;
    }
  }

  {
    String v = String(p_homeAssistant.getValue());
    v.trim();
    v.toLowerCase();
    // Accept: on/off, true/false, 1/0, yes/no.
    // If the field is left empty, keep the current setting.
    if (v.length() > 0) {
      bool on = (v == "on" || v == "true" || v == "1" || v == "yes");
      bool off = (v == "off" || v == "false" || v == "0" || v == "no");
      if (on) cfg.homeAssistantDiscovery = true;
      else if (off) cfg.homeAssistantDiscovery = false;
    }
  }

  {
    String t = String(p_apiToken.getValue());
    t.trim();
    // Portal shows this field blank by default:
    // - Empty: keep existing token
    // - "clear" / "off" / "-" : remove token (disable API writes)
    if (t.length() > 0) {
      String tl = t; tl.toLowerCase();
      if (tl == "clear" || tl == "off" || tl == "-") cfg.apiToken = "";
      else cfg.apiToken = t;
    }
  }

  cfg.mqttPublishIntervalSec = (uint16_t)atoi(p_interval.getValue());
  if (cfg.mqttPublishIntervalSec < 1) cfg.mqttPublishIntervalSec = 1;

  saveConfig();
  shouldSaveConfig = false;
}

// ---------- Shared head injection (CSS + JS) ----------
static const char* WM_CUSTOM_HEAD = R"rawliteral(
<style>
/* Hide the auto WiFiManager inputs for the dropdown-backed fields */
#dzmode, #homeafterrx, #ha { display:none !important; }

/* A bit nicer spacing for our custom controls */
.wmrow { margin: 10px 0; }
.wmlabel { display:block; margin: 0 0 6px 0; font-weight: 600; }
.wmselect { width: 100%; padding: 8px; }
.wmhint { font-size: 12px; opacity: 0.75; margin-top: 4px; }
</style>

<script>
document.addEventListener('DOMContentLoaded', function () {

  function syncSelect(selectId, hiddenId) {
    var sel = document.getElementById(selectId);
    var hid = document.getElementById(hiddenId);
    if (!sel || !hid) return;

    // Initialize select from hidden input
    var cur = (hid.value || '').trim().toLowerCase();
    if (cur) sel.value = cur;
    hid.value = sel.value;

    sel.addEventListener('change', function(){
      hid.value = sel.value;
    });
  }

  syncSelect('dzmode_sel', 'dzmode');
  syncSelect('homeafterrx_sel', 'homeafterrx');
  syncSelect('ha_sel', 'ha');

  // Token show/hide: keep single input, just toggle type
  var tok = document.getElementById('apitoken');
  if (tok) {
    tok.type = 'password';

    // Only add checkbox once
    if (!document.getElementById('showtoken')) {
      var wrap = document.createElement('div');
      wrap.style.marginTop = '6px';

      var cb = document.createElement('input');
      cb.type = 'checkbox';
      cb.id = 'showtoken';

      var lb = document.createElement('label');
      lb.htmlFor = 'showtoken';
      lb.style.marginLeft = '6px';
      lb.appendChild(document.createTextNode('Show token'));

      wrap.appendChild(cb);
      wrap.appendChild(lb);

      tok.parentNode.insertBefore(wrap, tok.nextSibling);

      cb.addEventListener('change', function(){
        tok.type = cb.checked ? 'text' : 'password';
      });
    }
  }
});
</script>
)rawliteral";

// ---------- Portal ----------
bool runWiFiManagerPortal(bool forcedPortal, uint16_t portalTimeoutSec) {
  loadConfig();

  static char deviceNameBuf[40];
  static char mqttHostBuf[64];
  static char mqttPortBuf[8];
  static char dzTempBuf[8];
  static char dzSetBuf[8];
  static char dzModeBuf[8];
  static char homeAfterRxBuf[8];
  static char haBuf[8];
  static char intervalBuf[8];
  static char apiTokenBuf[64];

  snprintf(deviceNameBuf, sizeof(deviceNameBuf), "%s", cfg.deviceName.c_str());
  snprintf(mqttHostBuf, sizeof(mqttHostBuf), "%s", cfg.mqttServer.c_str());
  snprintf(mqttPortBuf, sizeof(mqttPortBuf), "%u", (unsigned)cfg.mqttPort);
  if (cfg.domoticzIdxTemp > 0) snprintf(dzTempBuf, sizeof(dzTempBuf), "%d", cfg.domoticzIdxTemp); else dzTempBuf[0] = 0;
  if (cfg.domoticzIdxSetpoint > 0) snprintf(dzSetBuf, sizeof(dzSetBuf), "%d", cfg.domoticzIdxSetpoint); else dzSetBuf[0] = 0;
  snprintf(dzModeBuf, sizeof(dzModeBuf), "%s", cfg.domoticzOutMode.c_str());
  snprintf(homeAfterRxBuf, sizeof(homeAfterRxBuf), "%s", cfg.homingAfterReceivingSetpoint ? "on" : "off");
  snprintf(haBuf, sizeof(haBuf), "%s", cfg.homeAssistantDiscovery ? "on" : "off");
  snprintf(intervalBuf, sizeof(intervalBuf), "%u", (unsigned)cfg.mqttPublishIntervalSec);
  apiTokenBuf[0] = 0; // never prefill token in portal

  // Visible dropdown UI (NO <style> / <script> here!)
  static String ui_dzMode;
  static String ui_homeAfterRx;
  static String ui_ha;

  {
    String selIndex = (cfg.domoticzOutMode == "flat") ? "" : " selected";
    String selFlat  = (cfg.domoticzOutMode == "flat") ? " selected" : "";
    ui_dzMode =
      "<div class='wmrow'>"
      "<label class='wmlabel' for='dzmode_sel'>Domoticz gateway OUT mode</label>"
      "<select class='wmselect' id='dzmode_sel' name='dzmode_sel'>"
      "<option value='index'" + selIndex + ">index (default)</option>"
      "<option value='flat'"  + selFlat  + ">flat</option>"
      "</select>"
      "</div>";
  }

  {
    String selOn  = cfg.homingAfterReceivingSetpoint ? " selected" : "";
    String selOff = cfg.homingAfterReceivingSetpoint ? "" : " selected";
    ui_homeAfterRx =
      "<div class='wmrow'>"
      "<label class='wmlabel' for='homeafterrx_sel'>Homing after receiving setpoint</label>"
      "<select class='wmselect' id='homeafterrx_sel' name='homeafterrx_sel'>"
      "<option value='on'"  + selOn  + ">on (default)</option>"
      "<option value='off'" + selOff + ">off</option>"
      "</select>"
      "</div>";
  }

  {
    String selOn  = cfg.homeAssistantDiscovery ? " selected" : "";
    String selOff = cfg.homeAssistantDiscovery ? "" : " selected";
    ui_ha =
      "<div class='wmrow'>"
      "<label class='wmlabel' for='ha_sel'>Home Assistant MQTT Discovery</label>"
      "<select class='wmselect' id='ha_sel' name='ha_sel'>"
      "<option value='on'"  + selOn  + ">on (default)</option>"
      "<option value='off'" + selOff + ">off</option>"
      "</select>"
      "</div>";
  }

  // Regular params
  WiFiManagerParameter p_deviceName("devname", "Device name (default: NastyThermostatXXXXXX)", deviceNameBuf, 31);
  WiFiManagerParameter p_mqttServer("mqtt", "MQTT server (empty = MQTT off)", mqttHostBuf, 63);
  WiFiManagerParameter p_mqttPort("mqttport", "MQTT port (default 1883)", mqttPortBuf, 7);
  WiFiManagerParameter p_domTemp("dztemp", "Domoticz IDX temperature (empty = Domoticz off)", dzTempBuf, 7);
  WiFiManagerParameter p_domSet("dzset", "Domoticz IDX setpoint (empty = Domoticz off)", dzSetBuf, 7);

  // Hidden real fields (these will be submitted). UI is shown via custom HTML blocks.
  WiFiManagerParameter p_dzMode("dzmode", "", dzModeBuf, 7);
  WiFiManagerParameter p_homeAfterRx("homeafterrx", "", homeAfterRxBuf, 7);
  WiFiManagerParameter p_homeAssistant("ha", "", haBuf, 7);

  // UI blocks
  WiFiManagerParameter p_ui_dzMode(ui_dzMode.c_str());
  WiFiManagerParameter p_ui_homeAfterRx(ui_homeAfterRx.c_str());
  WiFiManagerParameter p_ui_ha(ui_ha.c_str());

  // Token = single real input (show/hide checkbox injected via head JS)
  WiFiManagerParameter p_apiToken("apitoken", "HTTP API token (leave empty to keep, or type 'clear' to remove)", apiTokenBuf, 63);

  WiFiManagerParameter p_interval("interval", "MQTT publish interval (seconds)", intervalBuf, 7);

  shouldSaveConfig = false;
  wm.setSaveConfigCallback(onSaveConfig);
  wm.setTitle(String(FW_NAME) + " " + FW_VERSION);
  wm.setConnectTimeout(15);
  wm.setConfigPortalTimeout(portalTimeoutSec);
  wm.setBreakAfterConfig(true);
  // Keep custom parameters on the same WiFi page so users always see all options.
  wm.setParamsPage(false);

  // ✅ THIS fixes “CSS/JS shown as text” + enables hiding of inputs
  wm.setCustomHeadElement(WM_CUSTOM_HEAD);

  wm.addParameter(&p_deviceName);
  wm.addParameter(&p_mqttServer);
  wm.addParameter(&p_mqttPort);
  wm.addParameter(&p_domTemp);
  wm.addParameter(&p_domSet);

  // Add UI blocks first (so they appear where you want), then the hidden real inputs
  wm.addParameter(&p_ui_dzMode);
  wm.addParameter(&p_dzMode);

  wm.addParameter(&p_ui_homeAfterRx);
  wm.addParameter(&p_homeAfterRx);

  wm.addParameter(&p_ui_ha);
  wm.addParameter(&p_homeAssistant);

  wm.addParameter(&p_apiToken);
  wm.addParameter(&p_interval);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(200);

  String apNameStr = portalApName();
  const char* apName = apNameStr.c_str();

  Serial.println();
  Serial.println("========================================");
  Serial.println("🔧 WIFI CONFIG PORTAL MODE");
  Serial.print("🔧 AP SSID: "); Serial.println(apName);
  Serial.println("🔧 Open: http://192.168.4.1");
  Serial.println("========================================");

  bool ok = false;
  if (forcedPortal) ok = wm.startConfigPortal(apName);
  else             ok = wm.autoConnect(apName);

  if (!ok) {
    if (shouldSaveConfig) {
      Serial.println("⚠️ Portal ended without WiFi, but Save was pressed -> saving parameters anyway.");
      applyPortalParamsAndSave(
        p_deviceName,
        p_mqttServer, p_mqttPort, p_domTemp, p_domSet,
        p_dzMode, p_homeAfterRx, p_homeAssistant, p_apiToken,
        p_interval
      );
      wm.stopConfigPortal();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      return true;
    }
    Serial.println("❌ Portal exit without WiFi connection (timeout/fail) and no Save.");
    return false;
  }

  applyPortalParamsAndSave(
    p_deviceName,
    p_mqttServer, p_mqttPort, p_domTemp, p_domSet,
    p_dzMode, p_homeAfterRx, p_homeAssistant, p_apiToken,
    p_interval
  );

  wm.stopConfigPortal();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);

  Serial.println("✅ Portal completed (WiFi ok) and config saved.");
  return true;
}

// ---------- Auto connect ----------
bool setupWiFiAuto() {
  loadConfig();

  static char deviceNameBuf[40];
  static char mqttHostBuf[64];
  static char mqttPortBuf[8];
  static char dzTempBuf[8];
  static char dzSetBuf[8];
  static char dzModeBuf[8];
  static char homeAfterRxBuf[8];
  static char haBuf[8];
  static char intervalBuf[8];
  static char apiTokenBuf[64];

  snprintf(deviceNameBuf, sizeof(deviceNameBuf), "%s", cfg.deviceName.c_str());
  snprintf(mqttHostBuf, sizeof(mqttHostBuf), "%s", cfg.mqttServer.c_str());
  snprintf(mqttPortBuf, sizeof(mqttPortBuf), "%u", (unsigned)cfg.mqttPort);
  if (cfg.domoticzIdxTemp > 0) snprintf(dzTempBuf, sizeof(dzTempBuf), "%d", cfg.domoticzIdxTemp); else dzTempBuf[0] = 0;
  if (cfg.domoticzIdxSetpoint > 0) snprintf(dzSetBuf, sizeof(dzSetBuf), "%d", cfg.domoticzIdxSetpoint); else dzSetBuf[0] = 0;
  snprintf(dzModeBuf, sizeof(dzModeBuf), "%s", cfg.domoticzOutMode.c_str());
  snprintf(homeAfterRxBuf, sizeof(homeAfterRxBuf), "%s", cfg.homingAfterReceivingSetpoint ? "on" : "off");
  snprintf(haBuf, sizeof(haBuf), "%s", cfg.homeAssistantDiscovery ? "on" : "off");
  snprintf(intervalBuf, sizeof(intervalBuf), "%u", (unsigned)cfg.mqttPublishIntervalSec);
  apiTokenBuf[0] = 0; // never prefill token in portal

  // Minimal params (if portal opens via autoConnect, head injection still applies)
  WiFiManagerParameter p_deviceName("devname", "Device name (default: NastyThermostatXXXXXX)", deviceNameBuf, 31);
  WiFiManagerParameter p_mqttServer("mqtt", "MQTT server (empty = MQTT off)", mqttHostBuf, 63);
  WiFiManagerParameter p_mqttPort("mqttport", "MQTT port (default 1883)", mqttPortBuf, 7);
  WiFiManagerParameter p_domTemp("dztemp", "Domoticz IDX temperature (empty = Domoticz off)", dzTempBuf, 7);
  WiFiManagerParameter p_domSet("dzset", "Domoticz IDX setpoint (empty = Domoticz off)", dzSetBuf, 7);

  WiFiManagerParameter p_dzMode("dzmode", "", dzModeBuf, 7);
  WiFiManagerParameter p_homeAfterRx("homeafterrx", "", homeAfterRxBuf, 7);
  WiFiManagerParameter p_homeAssistant("ha", "", haBuf, 7);

  // Simple UI blocks (same layout as portal)
  static String ui_dzMode;
  static String ui_homeAfterRx;
  static String ui_ha;

  {
    String selIndex = (cfg.domoticzOutMode == "flat") ? "" : " selected";
    String selFlat  = (cfg.domoticzOutMode == "flat") ? " selected" : "";
    ui_dzMode =
      "<div class='wmrow'>"
      "<label class='wmlabel' for='dzmode_sel'>Domoticz gateway OUT mode</label>"
      "<select class='wmselect' id='dzmode_sel' name='dzmode_sel'>"
      "<option value='index'" + selIndex + ">index (default)</option>"
      "<option value='flat'"  + selFlat  + ">flat</option>"
      "</select>"
      "</div>";
  }

  {
    String selOn  = cfg.homingAfterReceivingSetpoint ? " selected" : "";
    String selOff = cfg.homingAfterReceivingSetpoint ? "" : " selected";
    ui_homeAfterRx =
      "<div class='wmrow'>"
      "<label class='wmlabel' for='homeafterrx_sel'>Homing after receiving setpoint</label>"
      "<select class='wmselect' id='homeafterrx_sel' name='homeafterrx_sel'>"
      "<option value='on'"  + selOn  + ">on (default)</option>"
      "<option value='off'" + selOff + ">off</option>"
      "</select>"
      "</div>";
  }

  {
    String selOn  = cfg.homeAssistantDiscovery ? " selected" : "";
    String selOff = cfg.homeAssistantDiscovery ? "" : " selected";
    ui_ha =
      "<div class='wmrow'>"
      "<label class='wmlabel' for='ha_sel'>Home Assistant MQTT Discovery</label>"
      "<select class='wmselect' id='ha_sel' name='ha_sel'>"
      "<option value='on'"  + selOn  + ">on (default)</option>"
      "<option value='off'" + selOff + ">off</option>"
      "</select>"
      "</div>";
  }

  WiFiManagerParameter p_ui_dzMode(ui_dzMode.c_str());
  WiFiManagerParameter p_ui_homeAfterRx(ui_homeAfterRx.c_str());
  WiFiManagerParameter p_ui_ha(ui_ha.c_str());

  WiFiManagerParameter p_apiToken("apitoken", "HTTP API token (leave empty to keep, or type 'clear' to remove)", apiTokenBuf, 63);
  WiFiManagerParameter p_interval("interval", "MQTT publish interval (seconds)", intervalBuf, 7);

  shouldSaveConfig = false;
  wm.setSaveConfigCallback(onSaveConfig);
  wm.setTitle(String(FW_NAME) + " " + FW_VERSION);
  wm.setConnectTimeout(15);
  wm.setConfigPortalTimeout(180);
  wm.setBreakAfterConfig(true);
  // Keep custom parameters on the same WiFi page so users always see all options.
  wm.setParamsPage(false);

  wm.setCustomHeadElement(WM_CUSTOM_HEAD);

  wm.addParameter(&p_deviceName);
  wm.addParameter(&p_mqttServer);
  wm.addParameter(&p_mqttPort);
  wm.addParameter(&p_domTemp);
  wm.addParameter(&p_domSet);

  wm.addParameter(&p_ui_dzMode);
  wm.addParameter(&p_dzMode);

  wm.addParameter(&p_ui_homeAfterRx);
  wm.addParameter(&p_homeAfterRx);

  wm.addParameter(&p_ui_ha);
  wm.addParameter(&p_homeAssistant);

  wm.addParameter(&p_apiToken);
  wm.addParameter(&p_interval);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(200);

  String apNameStr = portalApName();
  bool ok = wm.autoConnect(apNameStr.c_str());
  if (!ok) return false;

  applyPortalParamsAndSave(
    p_deviceName,
    p_mqttServer, p_mqttPort, p_domTemp, p_domSet,
    p_dzMode, p_homeAfterRx, p_homeAssistant, p_apiToken,
    p_interval
  );

  wm.stopConfigPortal();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);

  return true;
}


// ===================== SERIAL BANNER ========================================
void printBanner() {
  Serial.println();
  Serial.println("============================================================");
  Serial.print(FW_NAME); Serial.print(" v"); Serial.println(FW_VERSION);
  Serial.print("WiFi IP: "); Serial.println(WiFi.localIP());
  Serial.print("MQTT: ");
  if (!mqttEnabled) {
    Serial.println("DISABLED");
  } else {
    Serial.print(cfg.mqttServer); Serial.print(":"); Serial.println(cfg.mqttPort);
  }
  Serial.print("Publish interval: "); Serial.print(cfg.mqttPublishIntervalSec); Serial.println(" sec");
  Serial.print("Device name: "); Serial.println(cfg.deviceName);
  Serial.print("Topic base: "); Serial.println(cfg.topicBase);
  Serial.print("Domoticz IDX Temp: "); Serial.println(cfg.domoticzIdxTemp);
  Serial.print("Domoticz IDX Setpoint: "); Serial.println(cfg.domoticzIdxSetpoint);
  Serial.print("HA Discovery: "); Serial.println(cfg.homeAssistantDiscovery ? "ON" : "OFF");
  Serial.println();
  Serial.println("HTTP API:");
  Serial.println("  GET  /api  -> status JSON");
  Serial.println("  POST /api  -> update (token required)");
  Serial.println("============================================================");
  Serial.println();
}

// ===================== STEPPER PROFILES =====================================
void setStepperProfileNormal() {
  stepper.setMaxSpeed(speedNormal / microstepFactor);
  stepper.setAcceleration(accelNormal / microstepFactor);
}
void setStepperProfileRotary() {
  stepper.setMaxSpeed(speedRotary / microstepFactor);
  stepper.setAcceleration(accelRotary / microstepFactor);
}
void setStepperProfileHoming() {
  stepper.setMaxSpeed(speedHoming / microstepFactor);
  stepper.setAcceleration(accelHoming / microstepFactor);
}

// ===================== SCREEN HELPERS =======================================
// Ensure the backlight is ON and reset the screen timeout timer.
// Used so the display stays on during motor movement (homing / API / MQTT / encoder).
void wakeScreen() {
  lastInteractionMs = millis();
  if (!screenWakeAllowed) return;
  if (!screenOn) {
    digitalWrite(TFT_BL, HIGH);
    screenOn = true;
  }
}

// ===================== SETUP ================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  // --- TFT init ---
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI);
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW); delay(100);
  digitalWrite(TFT_RST, HIGH); delay(100);
  tft.init(172, 320, 34);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);
  pinMode(TFT_BL, OUTPUT);
  // Boot: keep screen OFF. We'll turn it on after homing.
  digitalWrite(TFT_BL, LOW);
  screenOn = false;

  // --- Encoder pins ---
  pinMode(encoderPinA, INPUT_PULLUP);
  pinMode(encoderPinB, INPUT_PULLUP);
  pinMode(encoderButtonPin, INPUT_PULLUP);

  // Forced portal if button held continuously for 5 seconds at boot
  bool forcePortalButton = true;
  unsigned long t0 = millis();
  while (millis() - t0 < 5000) {
    if (digitalRead(encoderButtonPin) != LOW) {
      forcePortalButton = false;
      break;
    }
    delay(10);
  }

  attachInterrupt(digitalPinToInterrupt(encoderButtonPin), handleButton, FALLING);

  // --- Preferences ---
  prefs.begin("setpoint", false);

  Setpoint   = prefs.getFloat("setpoint", 18.5);
  temperatureOffset = prefs.getFloat("tempOffset", 0.0);

  stepSize   = prefs.getFloat("stepSize", 0.1);
  stepSizeIndex = 0;
  for (int i = 0; i < STEP_SIZE_COUNT; i++) {
    if (fabs(stepSizeOptions[i] - stepSize) < 0.001) { stepSizeIndex = i; break; }
  }

  stepsPerDegreeBase = prefs.getFloat("stapPerGraad", 26.75);
  stepsPerDegreeCalc  = stepsPerDegreeBase / microstepFactor;
  stepsPerDegreeNest  = stepsPerDegreeCalc;

  // --- DS18B20 ---
  primeTemperatureReading();

  // --- Stepper driver ---
  pinMode(enablePin, OUTPUT);
  // Start with the stepper driver disabled; we'll enable only when we actually move.
  disableStepperDriver();
  setStepperProfileNormal();

  // --- WiFi: decide portal mode ---
  bool portalRequested = consumePortalRequest();

  if (portalRequested || forcePortalButton) {
    // Portal/AP mode: keep motor and screen quiet.
    screenWakeAllowed = false;
    digitalWrite(TFT_BL, LOW);
    screenOn = false;
    disableStepperDriver();
    Serial.println(portalRequested ? "🔧 Portal requested from MENU (boot)" : "🔧 Forced portal (button held at boot)");
    bool ok = runWiFiManagerPortal(true, 180);
    Serial.println(ok ? "🔁 Portal finished (saved) -> reboot into normal mode..." : "🔁 Portal failed -> reboot into normal mode...");
    delay(300);
    ESP.restart();
    (void)ok;
  }

  // Normal boot connect
  bool wifiOk = setupWiFiAuto();
  if (!wifiOk) {
    // Portal/AP mode: keep motor and screen quiet.
    screenWakeAllowed = false;
    digitalWrite(TFT_BL, LOW);
    screenOn = false;
    disableStepperDriver();
    Serial.println("❌ WiFi not connected. Starting config portal (AP mode) - no reboot loop.");
    Serial.println("🔧 Connect to the AP and open: http://192.168.4.1");

    bool portalOk = runWiFiManagerPortal(true, 0); // 0 = no timeout
    if (portalOk) {
      Serial.println("✅ Portal saved -> rebooting into normal mode...");
      delay(300);
      ESP.restart();
    }

    Serial.println("⚠️ Portal exited without saving. Staying in AP mode; reboot manually when ready.");
    while (true) {
      delay(1000);
    }
  }


  // reload once more to be 100% sure banner+interval are correct
  loadConfig();

  printBanner();

  // Boot flow:
  // 1) Homing first (screen kept OFF)
  // 2) Then enable screen and move to the stored setpoint.
  screenWakeAllowed = false;
  homingNest(false);

  screenWakeAllowed = true;
  wakeScreen();
  drawInfo();
  setNestToTemperature(Setpoint);

  // --- MQTT ---
  setupMQTT();

  // publish setpoint once on boot after connect
  publishSetpointOnBootPending = true;

  // --- API ---
  server.on("/api", handleApiRequest);
  server.begin();

  drawInfo();
}

// ===================== LOOP =================================================
void loop() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 1) {
    checkEncoder();
    handleMenu();
    lastCheck = millis();
  }

  // Re-print banner after a short delay so Serial Monitor won't miss it
  static bool bannerReprinted = false;
  if (!bannerReprinted && millis() > 10000) {
    printBanner();
    bannerReprinted = true;
  }
  handleSerialCommands(); //? in serial shows banner again

  mqttLoop();
  server.handleClient();

// Outside the menu: adjust setpoint (only update value while turning)
// Outside the menu: adjust setpoint
static bool rotaryMoveActive = false;

// 1) While turning: update setpoint value and mark change time
if (!menuActive && !subMenuActive && encoderMovedSetpoint) {
  encoderMovedSetpoint = false;

  Setpoint += encoderDirection * stepSize;
  Setpoint = constrain(Setpoint, minNestTempC, maxNestTempC);

  lastSetpointChange = millis();
  pendingSetpointMove = true;
}

// 2) Start (or update) motion shortly after user pauses (fast, natural)
if (!menuActive && !subMenuActive && pendingSetpointMove &&
    (millis() - lastSetpointChange >= ENCODER_SETTLE_MS)) {

  // Start motion profile once
  if (!rotaryMoveActive) {
    setStepperProfileRotary();
    rotaryMoveActive = true;
  }

  // Always update target to latest Setpoint (no save yet)
  long targetSteps = lround((Setpoint - minNestTempC) * stepsPerDegreeNest);
  stepper.moveTo(targetSteps);

  pendingSetpointMove = false; // cleared until next encoder tick
}

// 3) Save setpoint after a longer pause (flash friendly)
if (!menuActive && !subMenuActive && (lastSetpointChange != 0) &&
    (millis() - lastSetpointChange > setpointSaveDelay)) {

  saveSetpoint();
  lastSetpointChange = 0;

  publishSetpoint(true);
  // keep rotary profile while moving; we switch back when done
}

// 4) When motion finished: restore normal profile
if (rotaryMoveActive && stepper.distanceToGo() == 0) {
  setStepperProfileNormal();
  rotaryMoveActive = false;
}

  // External setpoint state machine (MQTT/API/Domoticz)
  processExternalMoveState();

  // Temperature reading cycle
  if (!menuActive && !subMenuActive) {
    unsigned long now = millis();
    if (tempRequested && (now - lastTempRequest >= conversionTime)) {
      lastMeasuredTemp = readTemperature();
      hasTempReading = true;
      tempRequested = false;
      requestTemperature();
    }
  }

  // Stepper power management
  if (stepper.distanceToGo() != 0) {
    // Keep display on while the motor is moving (homing / API / encoder)
    wakeScreen();
    if (!stepperEnabled) {
      enableStepperDriver();
    }
    lastStepperMovement = millis();
  }

  stepper.run();

  if (stepperEnabled && (millis() - lastStepperMovement > stepperTimeout)) {
    disableStepperDriver();
  }

  // Screen backlight auto-off
  if (screenOn && (millis() - lastInteractionMs > SCREEN_TIMEOUT_MS)) {
    digitalWrite(TFT_BL, LOW);
    screenOn = false;
  }

  // Refresh main screen on change
  if (!menuActive && !subMenuActive) {
    bool wifiConn = (WiFi.status() == WL_CONNECTED);

    int sp10 = (int)lround(Setpoint * 10.0);
    int tm10 = (int)lround(lastMeasuredTemp * 10.0);
    int ls10 = (int)lround(lastShownSetpoint * 10.0);
    int lt10 = (int)lround(lastShownTemp * 10.0);

    if (sp10 != ls10 || tm10 != lt10 || wifiConn != lastWifiConnected) {
      drawInfo();
    }
  }
}

// ===================== ENCODER / BUTTON ====================================
void checkEncoder() {
  static uint8_t lastState = 0;
  static int8_t delta = 0;

  uint8_t clk = digitalRead(encoderPinA);
  uint8_t dt  = digitalRead(encoderPinB);
  uint8_t currentState = (clk << 1) | dt;

  if (currentState != lastState) {
    if (!screenOn) {
      lastInteractionMs = millis();
      digitalWrite(TFT_BL, HIGH);
      screenOn = true;
      lastState = currentState;
      delta = 0;
      return;
    }

    if ((lastState == 0b00 && currentState == 0b01) ||
        (lastState == 0b01 && currentState == 0b11) ||
        (lastState == 0b11 && currentState == 0b10) ||
        (lastState == 0b10 && currentState == 0b00)) {
      delta++;
    } else {
      delta--;
    }

    if (abs(delta) >= 2) {
      encoderDirection = (delta > 0) ? -1 : 1;
      lastInteractionMs = millis();

      if (menuActive || subMenuActive) encoderMovedMenu = true;
      else encoderMovedSetpoint = true;

      delta = 0;
    }
    lastState = currentState;
  }
}

void IRAM_ATTR handleButton() {
  static unsigned long lastPress = 0;
  unsigned long now = millis();

  if ((now - lastPress) > 250) {
    buttonPressed = true;
    lastPress = now;
  }
}

// ===================== TEMPERATURE =========================================
void requestTemperature() {
  ow.reset();
  ow.writeByte(0xCC);
  ow.writeByte(0x44);
  lastTempRequest = millis();
  tempRequested = true;
}

float readTemperature() {
  ow.reset();
  ow.writeByte(0xCC);
  ow.writeByte(0xBE);

  uint8_t data[9];
  for (int i = 0; i < 9; i++) data[i] = ow.readByte();

  int16_t raw = (data[1] << 8) | data[0];
  float tempC = raw / 16.0;

  return tempC + temperatureOffset;
}

// Do one blocking temperature conversion at boot so lastMeasuredTemp is valid
// before we publish HA "action" (heating/idle) and before the screen/UI comes up.
// This prevents a temporary 0.00°C from causing a false "heating" state.
void primeTemperatureReading() {
  requestTemperature();
  unsigned long start = millis();
  while (millis() - start < conversionTime) {
    delay(5);
  }
  lastMeasuredTemp = readTemperature();
  hasTempReading = true;

  // Start the normal async cycle again
  tempRequested = false;
  requestTemperature();
}


// ===================== FLASH OPSLAG ========================================
void saveSetpoint() {
  prefs.putFloat("setpoint", Setpoint);
  Serial.print("Setpoint saved: ");
  Serial.println(Setpoint, 1);
}

// ===================== STEPPER FUNCTIES =====================================
void homingNest(bool goToSetpoint) {
  // Keep display awake during homing (can be suppressed via screenWakeAllowed)
  wakeScreen();
  setStepperProfileHoming();

  long stepsForHoming = (long)(stepsPerNestRevolution * 0.75);
  delay(200);
  // 15-second timeout: prevents infinite run if the mechanical stop is missed.
  moveStepperTo(stepper.currentPosition() - stepsForHoming, 15000UL);

  stepper.setCurrentPosition(0);
  delay(200);

  setStepperProfileNormal();
  if (goToSetpoint) {
    setNestToTemperature(Setpoint);
  }
}

void setNestToTemperature(float targetTemp) {
  setStepperProfileNormal();

  targetTemp = constrain(targetTemp, minNestTempC, maxNestTempC);
  float currentTempC = minNestTempC + (stepper.currentPosition() / stepsPerDegreeNest);

  float deltaTempC = targetTemp - currentTempC;
  long steps = lround(deltaTempC * stepsPerDegreeNest);
  if (steps != 0) moveStepperTo(stepper.currentPosition() + steps);
}

void moveStepperTo(long absolutePosition, unsigned long timeoutMs) {
  // Any motor movement should wake the display (even without user interaction)
  // IMPORTANT:
  // We intentionally avoid heavy TFT redraws while stepping.
  // A full-screen redraw (fillScreen + big text) can take multiple milliseconds
  // over SPI and will starve AccelStepper::run(), causing jerky movement.
  wakeScreen();
  // Show something *once* before the move starts.
  if (!menuActive && !subMenuActive) {
    drawInfo();
  }
  if (!stepperEnabled) {
    enableStepperDriver();
  }
  stepper.moveTo(absolutePosition);
  // Custom run loop so we can keep the display awake during the (blocking) move.
  unsigned long lastYieldUs = micros();
  unsigned long startMs = millis();
  while (stepper.distanceToGo() != 0) {
    stepper.run();
    wakeScreen();

    // Give the WiFi stack some time, but NOT on every iteration.
    // Yielding too often can introduce tiny pauses during long moves.
    unsigned long nowUs = micros();
    if ((nowUs - lastYieldUs) > 2000UL) { // ~2ms
      yield();
      lastYieldUs = nowUs;
    }

    // Safety timeout: abort if the move takes too long (e.g. mechanical stop missed).
    if (timeoutMs > 0 && (millis() - startMs) >= timeoutMs) {
      stepper.stop();
      Serial.println("⚠️ moveStepperTo: timeout reached, stopping motor.");
      break;
    }
  }
  lastStepperMovement = millis();
}

// Apply a setpoint coming from an external source (MQTT/API/Domoticz).
// - With homingAfterReceivingSetpoint==on: do a full homing first (most precise).
// - With homingAfterReceivingSetpoint==off: do a small "wake" jiggle (0.5°C down & back)
//   before moving to the new setpoint, so the Nest wakes up.
void applyExternalSetpoint(float newSetpoint, const char* sourceTag) {
  newSetpoint = constrain(newSetpoint, minNestTempC, maxNestTempC);
  float oldSetpoint = Setpoint;

  if (fabs(newSetpoint - oldSetpoint) < 0.001f) {
    return; // no change
  }

  // Update the logical setpoint immediately (UI/API), movement will follow.
  Setpoint = newSetpoint;
  saveSetpoint();
  drawInfo();

  // IMPORTANT: publish the new setpoint immediately so HA/Domoticz and
  // NastyThermostat/out/setpoint always reflect the latest value.
  // If MQTT is not connected yet, queue a publish right after reconnect.
  if (mqttEnabled) {
    if (client.connected()) {
      publishSetpoint(true);
    } else {
      publishSetpointOnBootPending = true;
    }
  }


  if (cfg.homingAfterReceivingSetpoint) {
    Serial.printf("%s setpoint -> %.1f (homing first)\n", sourceTag, newSetpoint);
    homingNest();
    return;
  }

  // Wake-jiggle path (no homing): old -> (old-0.5°C) -> old -> new
  Serial.printf("%s setpoint -> %.1f (wake jiggle)\n", sourceTag, newSetpoint);

  float downTemp = oldSetpoint - 0.5f;
  if (downTemp < minNestTempC) {
    // If we're already near min, jiggle up instead.
    downTemp = minNestTempC + 0.5f;
    if (downTemp > maxNestTempC) downTemp = maxNestTempC;
  }

  // Convert temperatures to absolute stepper positions based on current calibration.
  auto tempToPos = [&](float t) -> long {
    t = constrain(t, minNestTempC, maxNestTempC);
    float rel = (t - minNestTempC);
    return lround(rel * stepsPerDegreeNest);
  };

  extTargetDown = tempToPos(downTemp);
  extTargetBack = tempToPos(oldSetpoint);
  extTargetNew  = tempToPos(newSetpoint);

  // Start state machine; actual stepping is handled in loop() to stay responsive.
  // Use homing profile for the quick wake-jiggle so it feels snappy/natural.
  setStepperProfileHoming();
  extMoveState = EXT_WAKE_DOWN;
  wakeScreen();
  if (!stepperEnabled) {
    enableStepperDriver();
  }
  stepper.moveTo(extTargetDown);
}

void processExternalMoveState() {
  if (extMoveState == EXT_NONE) return;

  // Keep screen awake while we are moving.
  if (stepper.distanceToGo() != 0) {
    wakeScreen();
    return;
  }

  // We reached the current target; advance.
  switch (extMoveState) {
    case EXT_WAKE_DOWN:
      extMoveState = EXT_WAKE_BACK;
      stepper.moveTo(extTargetBack);
      break;
    case EXT_WAKE_BACK:
      // After the quick wake wiggle, go back to normal profile
      setStepperProfileNormal();
      extMoveState = EXT_MOVE_NEW;
      stepper.moveTo(extTargetNew);
      break;
    case EXT_MOVE_NEW:
      extMoveState = EXT_NONE;
      break;
    default:
      extMoveState = EXT_NONE;
      break;
  }
}

// ===================== MENU LOGICA  ===============================
void handleMenu() {
  // --- Button click ---
  if (buttonPressed) {
    buttonPressed = false;

    // Wake screen only
    if (!screenOn) {
      lastInteractionMs = millis();
      digitalWrite(TFT_BL, HIGH);
      screenOn = true;
      return;
    }
    lastInteractionMs = millis();

    // If we're in a submenu, handle "confirm/back" behavior
    if (menuActive && subMenuActive) {
      if (menuIndex == 1) {
        // Save temperature offset
        temperatureOffset = offsetMenuValue;
        prefs.putFloat("tempOffset", temperatureOffset);
        subMenuActive = false;
        drawMenu();
        return;

      } else if (menuIndex == 2) {
        // Save steps/degree (calibrationStepsValue is in microsteps)
        stepsPerDegreeBase = calibrationStepsValue * microstepFactor;
        prefs.putFloat("stapPerGraad", stepsPerDegreeBase);

        stepsPerDegreeCalc = stepsPerDegreeBase / microstepFactor;
        stepsPerDegreeNest = stepsPerDegreeCalc;

        subMenuActive = false;
        drawMenu();
        return;

      } else if (menuIndex == 3) {
        // Save step size
        stepSize = stepSizeOptions[stepSizeIndex];
        prefs.putFloat("stepSize", stepSize);

        subMenuActive = false;
        drawMenu();
        return;

      } else if (menuIndex == 4) {
        // WiFi submenu
        if (wifiSubIndex == 0) {
          Serial.println("🔧 Starting WiFi portal from menu (reboot into portal)...");
          requestPortalAndReboot();
          return;
        } else {
          subMenuActive = false;
          drawMenu();
          return;
        }

      } else if (menuIndex == 5) {
        // Status screen: any press = back
        subMenuActive = false;
        drawMenu();
        return;
      }
    }

    // If menu is active (and not in submenu), select menu item
    if (menuActive) {
      switch (menuIndex) {
        case 0: // Homing
          menuActive = false;
          tft.fillScreen(ST77XX_BLACK);
          tft.setTextSize(2);
          tft.setTextColor(ST77XX_WHITE);
          tft.setCursor(10, 60);
          tft.print("Homing...");
          homingNest();
          drawInfo();
          break;

        case 1: // Temp offset
          subMenuActive = true;
          offsetMenuValue = temperatureOffset;
          drawSubmenuOffset();
          break;

        case 2: // Steps/degree
          subMenuActive = true;
          calibrationStepsValue = stepsPerDegreeBase / microstepFactor; // show in microsteps
          drawSubmenuSteps();
          break;

        case 3: // Step size
          subMenuActive = true;
          drawSubmenuStepSize();
          break;

        case 4: // WiFi setup
          subMenuActive = true;
          wifiSubIndex = 0;
          drawSubmenuWiFi();
          break;

        case 5: // Status
          subMenuActive = true;
          drawSubmenuStatus();
          break;

        case 6: // Back
          menuActive = false;
          drawInfo();
          break;
      }
      return;
    }

    // Menu not active -> open it
    menuActive = true;
    menuIndex = 0;
    drawMenu();
    return;
  }

  // --- Encoder movement inside menu/submenu ---
  if (encoderMovedMenu) {
    encoderMovedMenu = false;

    if (menuActive && !subMenuActive) {
      menuIndex += encoderDirection;
      if (menuIndex < 0) menuIndex = menuItemCount - 1;
      if (menuIndex >= menuItemCount) menuIndex = 0;
      drawMenu();
      return;
    }

    if (menuActive && subMenuActive) {
      if (menuIndex == 1) {
        // Offset submenu
        offsetMenuValue += encoderDirection * 0.1f;
        offsetMenuValue = constrain(offsetMenuValue, -10.0f, 10.0f);
        drawSubmenuOffset();

      } else if (menuIndex == 2) {
        // Steps/degree submenu (microsteps)
        calibrationStepsValue += encoderDirection * 1.0f;
        calibrationStepsValue = constrain(calibrationStepsValue, 10.0f, 2000.0f);
        drawSubmenuSteps();

      } else if (menuIndex == 3) {
        // Step size submenu
        stepSizeIndex += encoderDirection;
        if (stepSizeIndex < 0) stepSizeIndex = STEP_SIZE_COUNT - 1;
        if (stepSizeIndex >= STEP_SIZE_COUNT) stepSizeIndex = 0;
        drawSubmenuStepSize();

      } else if (menuIndex == 4) {
        // WiFi submenu selection
        wifiSubIndex += encoderDirection;
        if (wifiSubIndex < 0) wifiSubIndex = wifiSubCount - 1;
        if (wifiSubIndex >= wifiSubCount) wifiSubIndex = 0;
        drawSubmenuWiFi();

      } else if (menuIndex == 5) {
        // Status screen: no encoder actions
      }
      return;
    }
  }
}

void drawInfo() {
  bool heatingOn = Setpoint > lastMeasuredTemp;
  uint16_t bg = heatingOn ? ST77XX_ORANGE : ST77XX_BLACK;
  tft.fillScreen(bg);

  tft.setTextSize(8);
  tft.setTextColor(ST77XX_WHITE);
  int16_t x1, y1; uint16_t w, h;
  char spBuf[10];
  dtostrf(Setpoint, 4, 1, spBuf);
  tft.getTextBounds(spBuf, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((320 - w) / 2, (172 - h) / 2);
  tft.print(spBuf);

  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  char tBuf[16];
  dtostrf(lastMeasuredTemp, 4, 1, tBuf);
  strcat(tBuf, " C");
  tft.setCursor(10, 130);
  tft.print(tBuf);

  bool wifiConn = (WiFi.status() == WL_CONNECTED);
  drawWifiIcon(wifiConn);

  lastShownSetpoint = Setpoint;
  lastShownTemp     = lastMeasuredTemp;
  lastWifiConnected = wifiConn;
}

void drawMenu() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);

  // Header
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 2);
  String header = cfg.deviceName;
  header.trim();
  if (header.length() == 0) header = FW_NAME;

  int16_t hx, hy; uint16_t hw, hh;
  tft.getTextBounds(header, 0, 0, &hx, &hy, &hw, &hh);
  if (hw > (uint16_t)(tft.width() - 20)) {
    while (header.length() > 1) {
      String candidate = header.substring(0, header.length() - 1) + "...";
      tft.getTextBounds(candidate, 0, 0, &hx, &hy, &hw, &hh);
      if (hw <= (uint16_t)(tft.width() - 20)) {
        header = candidate;
        break;
      }
      header.remove(header.length() - 1);
    }
  }
  tft.print(header);

  const int screenW = tft.width();
  const int screenH = tft.height();

  const int top = 18;         
  const int bottomMargin = 4;  

  int available = screenH - top - bottomMargin;
  int lineH = available / menuItemCount;

  if (lineH < 18) lineH = 18;

  const int textH = 16; 
  const int indentX = 10;

  for (int i = 0; i < menuItemCount; i++) {
    int y = top + i * lineH;

    // Highlight
    if (i == menuIndex) {
      tft.fillRect(0, y, screenW, lineH, ST77XX_YELLOW);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.setTextColor(ST77XX_WHITE);
    }
    int yText = y + (lineH - textH) / 2;
    if (yText < y) yText = y;

    tft.setCursor(indentX, yText);
    tft.print(menuItems[i]);
  }

  drawWifiIcon(WiFi.status() == WL_CONNECTED);
}

void drawSubmenuOffset() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);

  tft.setCursor(10, 10);
  tft.print("Temperature offset");

  tft.setCursor(10, 66);
  tft.print("Offset:  ");
  tft.print(offsetMenuValue, 1);
  tft.print(" C");

  tft.setCursor(10, 100);
  tft.print("Turn = adjust");
  tft.setCursor(10, 124);
  tft.print("Press = save");

  drawWifiIcon(WiFi.status() == WL_CONNECTED);
}

void drawSubmenuSteps() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);

  tft.setCursor(10, 10);
  tft.print("Steps/degree (microsteps)");

  tft.setCursor(10, 40);
  tft.print("Current: ");
  tft.print(calibrationStepsValue, 1);

  tft.setCursor(10, 74);
  tft.print("Turn = adjust");
  tft.setCursor(10, 98);
  tft.print("Press = save");

  drawWifiIcon(WiFi.status() == WL_CONNECTED);
}

void drawSubmenuStepSize() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);

  tft.setCursor(10, 10);
  tft.print("Step size");

  tft.setCursor(10, 40);
  tft.print("Current: ");
  tft.print(stepSizeOptions[stepSizeIndex], 1);
  tft.print(" C");

  tft.setCursor(10, 74);
  tft.print("Turn = adjust");
  tft.setCursor(10, 98);
  tft.print("Press = save");

  drawWifiIcon(WiFi.status() == WL_CONNECTED);
}

void drawSubmenuWiFi() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);

  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 10);
  tft.print("WiFi setup");

  for (int i = 0; i < wifiSubCount; i++) {
    int y = 45 + i * 24;
    if (i == wifiSubIndex) {
      tft.fillRect(0, y, 320, 24, ST77XX_YELLOW);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.setTextColor(ST77XX_WHITE);
    }
    tft.setCursor(10, y + 2);
    tft.print(wifiSubItems[i]);
  }

  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 110);
  tft.print("AP: ");
  tft.print(portalApName());
  tft.setCursor(10, 134);
  tft.print("Captive portal at 192.168.4.1");

  drawWifiIcon(WiFi.status() == WL_CONNECTED);
}

void drawSubmenuStatus() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);

  // Title
  tft.setCursor(10, 10);
  tft.print("STATUS");

  // Firmware
  tft.setCursor(10, 36);
  tft.print("FW: ");
  tft.print(FW_VERSION);

  // WiFi status
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  tft.setCursor(10, 62);
  tft.print("WiFi: ");
  tft.print(wifiOk ? "Connected" : "Disconnected");

  // IP address
  tft.setCursor(10, 86);
  tft.print("IP: ");
  if (wifiOk) {
    tft.print(WiFi.localIP());
  } else {
    tft.print("-");
  }

  // MQTT status
  tft.setCursor(10, 110);
  tft.print("MQTT: ");
  if (!mqttEnabled) {
    tft.print("Disabled");
  } else {
    tft.print(client.connected() ? "Connected" : "Disconnected");
  }

  // Hint
  tft.setCursor(10, 140);
  tft.print("Press = back");

  drawWifiIcon(wifiOk);
}



void drawWifiIcon(bool connected) {
  int x = 300;
  int y = 10;
  uint16_t color = connected ? ST77XX_GREEN : ST77XX_RED;

  tft.drawLine(x - 8, y + 0, x + 0, y + 0, color);
  tft.drawLine(x - 6, y + 2, x - 2, y + 2, color);
  tft.drawLine(x - 5, y + 4, x - 3, y + 4, color);
  tft.fillRect(x - 4, y + 6, 1, 1, color);
}

// ===================== MQTT ================================================

// Helper: Domoticz OUT topic based on mode + idx
static String getDomoticzOutTopic() {
  if (cfg.domoticzOutMode == "flat") return String("domoticz/out");
  // index mode
  if (cfg.domoticzIdxSetpoint <= 0) return String("");
  return String("domoticz/out/") + String(cfg.domoticzIdxSetpoint);
}

void setupMQTT() {
  if (!mqttEnabled) {
    Serial.println("ℹ️ MQTT is disabled (no server configured).");
    return;
  }

  // Domoticz/out  > 256 bytes 
  client.setBufferSize(1024);

  client.setServer(cfg.mqttServer.c_str(), cfg.mqttPort);
  client.setCallback(mqttCallback);

  reconnectMQTT();
}

void reconnectMQTT() {
  if (!mqttEnabled) return;

  while (!client.connected()) {
    Serial.print("🔌 MQTT connecting as clientId='");
    Serial.print(cfg.deviceName);
    Serial.print("' to ");
    Serial.print(cfg.mqttServer);
    Serial.print(":");
    Serial.println(cfg.mqttPort);

    // Use Home Assistant availability topic as MQTT Last Will.
    if (client.connect(cfg.deviceName.c_str(), haTopicAvailability().c_str(), 0, true, "offline")) {
      Serial.println("✅ MQTT connected");

      Serial.print("📩 Subscribing to: ");
      String dzTopic = getDomoticzOutTopic();
      Serial.println(dzTopic);

      bool ok = false;
      if (cfg.domoticzIdxSetpoint > 0) {
        ok = client.subscribe(dzTopic.c_str());
      } else {
        Serial.println("ℹ️ Domoticz setpoint IDX not configured -> no subscribe");
        ok = true;
      }
      Serial.print("📩 Subscribe result: ");
      Serial.println(ok ? "OK" : "FAILED");

      // Generic command topic (direct setpoint control)
      if (cfg.topicSetpointCmd.length() > 0) {
        Serial.print("📡 Subscribing cmd: ");
        Serial.println(cfg.topicSetpointCmd);
        bool okCmd = client.subscribe(cfg.topicSetpointCmd.c_str());
        Serial.print("📡 Cmd subscribe result: ");
        Serial.println(okCmd ? "OK" : "FAILED");
      }

      // Always publish availability online on connect (retained).
      publishAvailability(true);

      // Home Assistant discovery (retained)
      if (cfg.homeAssistantDiscovery) {
        publishHomeAssistantDiscovery();
        publishHomeAssistantModeHeat();
        publishHomeAssistantAction();
      }

      // publish setpoint once after connect
      publishSetpointOnBootPending = true;
    } else {
      Serial.print("❌ MQTT connect failed, rc=");
      Serial.println(client.state());
      delay(2000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("📩 MQTT message arrived: ");
  Serial.print(topic);
  Serial.print(" | ");

  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.println(msg);

  // Direct setpoint command topic (generic)
  if (String(topic) == cfg.topicSetpointCmd) {
    char* endPtr = nullptr;
    double v = strtod(msg.c_str(), &endPtr);
    if (endPtr == msg.c_str()) {
      Serial.println("❌ Cmd payload is not a number");
      return;
    }
    float newSetpoint = (float)v;
    newSetpoint = constrain(newSetpoint, 9.0f, 32.0f);
    Serial.print("🎯 New setpoint from MQTT cmd: ");
    Serial.println(newSetpoint, 2);

    // Guard against very fast "echo" patterns (e.g. some automation copying
    // state -> command). If the cmd equals the setpoint we just published
    // moments ago, ignore.
    if (fabs(newSetpoint - Setpoint) < 0.001f &&
        (millis() - lastSetpointStatePublishMs) < SETPOINT_ECHO_GUARD_MS) {
      Serial.println("↩️ Ignoring MQTT cmd echo (matches just-published state)");
      return;
    }

    applyExternalSetpoint(newSetpoint, "MQTT cmd");
    return;
  }

  // Domoticz setpoint intake (optional)
  if (cfg.domoticzIdxSetpoint <= 0) return;

  String t(topic);
  String expected = (cfg.domoticzOutMode == "flat") ? String("domoticz/out") : getDomoticzOutTopic();
  if (t != expected) return;

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    Serial.print("❌ JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  // In flat mode we MUST match idx, in index mode it's optional
  if (cfg.domoticzOutMode == "flat") {
    if (!doc.containsKey("idx")) {
      Serial.println("❌ Flat mode: no 'idx' in payload");
      return;
    }
  }
  if (doc.containsKey("idx")) {
    int idx = doc["idx"].as<int>();
    if (idx != cfg.domoticzIdxSetpoint) {
      Serial.print("ℹ️ Ignoring idx ");
      Serial.println(idx);
      return;
    }
  }

  if (!doc.containsKey("svalue1")) {
    Serial.println("❌ No 'svalue1' in payload");
    return;
  }

  float newSetpoint = doc["svalue1"].as<float>();
  Serial.print("🎯 New setpoint from Domoticz: ");
  Serial.println(newSetpoint, 2);

  // Ignore fast echoes of our own publish (prevents feedback/loops)
  if (fabs(newSetpoint - lastSentDomoticzSetpoint) < 0.001f &&
      (millis() - lastSentDomoticzSetpointMs) < SETPOINT_ECHO_GUARD_MS) {
    Serial.println("↩️ Ignoring Domoticz echo of our own setpoint publish");
    return;
  }

  applyExternalSetpoint(newSetpoint, "Domoticz");
}

void mqttLoop() {
  if (!mqttEnabled) return;

  if (!client.connected()) reconnectMQTT();
  client.loop();

  // publish setpoint once on boot/after reconnect
  if (publishSetpointOnBootPending && client.connected()) {
    publishSetpoint(true);
    publishSetpointOnBootPending = false;
  }

  // temperature publish on interval
  unsigned long now = millis();
  if (now - lastMqttPublish >= mqttInterval) {
    lastMqttPublish = now;
    publishTemperature();
  }
}

// ===================== PUBLISH =============================================
void publishTemperature() {
  if (!mqttEnabled || !client.connected()) return;
  // Keep Home Assistant availability alive by publishing online together with
  // the telemetry interval (matches cfg.mqttPublishIntervalSec).
  publishAvailability(true);
  char buffer[16];
  dtostrf(lastMeasuredTemp, 4, 2, buffer);
  client.publish(cfg.topicTemp.c_str(), buffer);
  publishDomoticzTemp();

  // Keep HA entity available. This piggybacks on the user-configured publish interval.
  publishAvailability(true);

  // Update HA action when we have fresh temperature.
  publishHomeAssistantAction();
}

void publishSetpoint(bool force) {
  if (!mqttEnabled || !client.connected()) return;
  if (force || fabs(Setpoint - lastPublishedSetpoint) > 0.001) {
    // Any user-visible update implies we are online.
    publishAvailability(true);
    char setpointStr[16];
    dtostrf(Setpoint, 4, 2, setpointStr);
    // For Home Assistant we publish the setpoint as retained state, so HA
    // won't jump back to an older retained value after reconnect.
    bool retain = cfg.homeAssistantDiscovery;
    client.publish(cfg.topicSetpoint.c_str(), setpointStr, retain);

    // Remember when we last published the state setpoint. 
    lastSetpointStatePublishMs = millis();

    publishDomoticzSetpoint();
    lastPublishedSetpoint = Setpoint;

    // Also refresh availability immediately on user interaction / external updates.
    publishAvailability(true);

    // Action can change when the setpoint changes.
    publishHomeAssistantAction();
  }
}

void publishDomoticzTemp() {
  if (!mqttEnabled || cfg.domoticzIdxTemp <= 0) return;

  if (!mqttEnabled || !client.connected()) return;
  char tempStr[16];
  dtostrf(lastMeasuredTemp, 4, 2, tempStr);

  String message = String("{\"idx\": ") + cfg.domoticzIdxTemp +
                   ", \"nvalue\": 0, \"svalue\": \"" + tempStr +
                   "\", \"source\": \"stepper\"}";

  client.publish(cfg.topicDomoticzIn.c_str(), message.c_str());
  lastPublishedTemp = lastMeasuredTemp;
}

void publishDomoticzSetpoint() {
  if (!mqttEnabled || cfg.domoticzIdxSetpoint <= 0) return;

  if (!mqttEnabled || !client.connected()) return;
  char setpointStr[16];
  dtostrf(Setpoint, 4, 2, setpointStr);

  String message = String("{\"idx\": ") + cfg.domoticzIdxSetpoint +
                   ", \"nvalue\": 0, \"svalue\": \"" + setpointStr +
                   "\", \"source\": \"stepper\"}";

  client.publish(cfg.topicDomoticzIn.c_str(), message.c_str());

  // Store for echo suppression (Domoticz often reflects this back via domoticz/out)
  lastSentDomoticzSetpoint = Setpoint;
  lastSentDomoticzSetpointMs = millis();
}


// ===================== HOME ASSISTANT (MQTT DISCOVERY) ======================
static String haDeviceId() {
  // Use MAC without separators as stable unique id
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  if (mac.length() == 0) mac = String("nasty") + String((uint32_t)ESP.getEfuseMac(), HEX);
  return mac;
}

static String haTopicAvailability() {
  return cfg.topicBase + "/out/availability";
}

static String haTopicModeState() {
  return cfg.topicBase + "/out/mode";
}

static String haTopicModeCommand() {
  return cfg.topicBase + "/in/mode";
}

static String haTopicAction() {
  // HVAC action (heating/idle)
  return cfg.topicBase + "/out/action";
}

void publishAvailability(bool online) {
  // Availability is useful even if HA discovery was toggled off after the entity
  // was created (otherwise HA can show "unavailable" after a reconnect/LWT).
  if (!mqttEnabled || !client.connected()) return;
  const char* payload = online ? "online" : "offline";
  client.publish(haTopicAvailability().c_str(), payload, true); // retained
}

void publishHomeAssistantModeHeat() {
  if (!mqttEnabled || !client.connected() || !cfg.homeAssistantDiscovery) return;
  client.publish(haTopicModeState().c_str(), "heat", true); // retained
}

void publishHomeAssistantAction() {
  if (!mqttEnabled || !client.connected() || !cfg.homeAssistantDiscovery) return;

  // Don't publish action until we have at least one valid temperature reading.
  if (!hasTempReading) return;

  // Determine action based on current temp vs setpoint.
  // Small deadband to avoid flapping.
  const float deadband = 0.1f;
  const char* action = (Setpoint > (lastMeasuredTemp + deadband)) ? "heating" : "idle";

  // Publish if it changed OR periodically refresh (so MQTT tools show updates
  // and HA always has a fresh retained state after broker restarts).
  unsigned long now = millis();
  if (lastPublishedAction != action || (now - lastActionPublishMs >= mqttInterval)) {
    client.publish(haTopicAction().c_str(), action, true); // retained
    lastPublishedAction = action;
    lastActionPublishMs = now;
  }
}

void publishHomeAssistantDiscovery() {
  if (!mqttEnabled || !client.connected() || !cfg.homeAssistantDiscovery) return;

  // One MQTT Climate entity: shows current temperature + target setpoint
  String devId = haDeviceId();
  String uniqueId = devId + "_climate";
  String objectId = topicSafeBaseFromName(cfg.deviceName) + "_thermostat";
  objectId.toLowerCase();

  String discoveryTopic = String("homeassistant/climate/") + uniqueId + "/config";

  StaticJsonDocument<1024> doc;

  // ✅ Entity name (what you see as the control name)
  // Set this DIFFERENT from the device name to avoid "Device Device" in HA UI.
  doc["name"] = HA_CLIMATE_NAME;
  doc["has_entity_name"] = true;

  doc["uniq_id"] = uniqueId;
  doc["obj_id"] = objectId;

  // Availability
  doc["avty_t"] = haTopicAvailability();
  doc["pl_avail"] = "online";
  doc["pl_not_avail"] = "offline";

  // Climate topics
  doc["curr_temp_t"] = cfg.topicTemp;          // current temp
  doc["temp_stat_t"] = cfg.topicSetpoint;      // target temp state
  doc["temp_cmd_t"]  = cfg.topicSetpointCmd;   // set target temp

  // Optional: report whether we're actively heating or idle
  doc["act_t"] = haTopicAction();

  doc["temp_unit"] = "C";
  doc["min_temp"] = minNestTempC;
  doc["max_temp"] = maxNestTempC;
  doc["temp_step"] = 0.1;

  // Simple single-mode 'heat'
  JsonArray modes = doc.createNestedArray("modes");
  modes.add("heat");
  doc["mode_stat_t"] = haTopicModeState();
  doc["mode_cmd_t"]  = haTopicModeCommand();

  // Device info (this is the physical device in HA)
  JsonObject dev = doc.createNestedObject("dev");
  JsonArray ids = dev.createNestedArray("ids");
  ids.add(devId);

  // ✅ Device name (the device header/group in HA)
  dev["name"] = haPrettyDeviceName();

  dev["mdl"] = HA_CLIMATE_NAME;
  dev["mf"] = "KB";
  dev["sw"] = FW_VERSION;

  String payload;
  serializeJson(doc, payload);

  client.publish(discoveryTopic.c_str(), payload.c_str(), true); // retained

  // Keep core retained state topics sane
  publishAvailability(true);
  publishHomeAssistantModeHeat();
  publishHomeAssistantAction();
}

//bij ? in serial opnieuw banner weergeven
void handleSerialCommands() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();

    if (c == '?' || c == 'h') {
      printBanner();
      Serial.println("Commands:");
      Serial.println("  ? or h  -> show this help/status");
      Serial.println("  i       -> show IP + MQTT status");
      Serial.println();
    } else if (c == 'i') {
      Serial.print("WiFi: ");
      Serial.println(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("MQTT: ");
      if (!mqttEnabled) Serial.println("Disabled");
      else Serial.println(client.connected() ? "Connected" : "Disconnected");
    }
  }
}

// ===================== API (HTTP) ==========================================
void handleApiRequest() {
  // ---------- GET: status ----------
  if (server.method() == HTTP_GET) {
    StaticJsonDocument<768> doc;
    doc["device_name"] = cfg.deviceName;
    doc["topic_base"] = cfg.topicBase;
    doc["setpoint"] = Setpoint;
    doc["temperature"] = lastMeasuredTemp;
    doc["offset"] = temperatureOffset;
    doc["stepSize"] = stepSize;
    doc["motor_position"] = stepper.currentPosition();

    // ✅ steps/degree (microsteps + fullstep storage)
    doc["steps_per_degree"] = stepsPerDegreeNest;        // microsteps/°C (runtime)
    doc["steps_per_degree_fullstep"] = stepsPerDegreeBase; // fullstep basis (stored)

    doc["mqtt_server"] = cfg.mqttServer;
    doc["mqtt_port"] = cfg.mqttPort;
    doc["mqtt_enabled"] = mqttEnabled;
    doc["domoticz_idx_temp"] = cfg.domoticzIdxTemp;
    doc["domoticz_idx_setpoint"] = cfg.domoticzIdxSetpoint;
    doc["domoticz_out_mode"] = cfg.domoticzOutMode;
    doc["mqtt_interval_sec"] = cfg.mqttPublishIntervalSec;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
    return;
  }

  // ---------- POST: wijzigingen ----------
  if (server.method() == HTTP_POST) {
    StaticJsonDocument<768> doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
      server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
      return;
    }

    // Token check (use token from AP settings / Preferences: cfg.apiToken)
    cfg.apiToken.trim();

    // If no token is configured via portal, block writes explicitly
    if (cfg.apiToken.length() == 0) {
      server.send(403, "application/json", "{\"error\":\"API writes disabled (set token in portal)\"}");
      return;
    }

    // Require token in POST body: { "token": "..." }
    if (!doc.containsKey("token")) {
      server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    String provided = String((const char*)doc["token"]);
    provided.trim();

    if (provided != cfg.apiToken) {
      server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }


    bool changed = false;

    if (doc.containsKey("setpoint")) {
      float sp = doc["setpoint"];
      if (sp >= minNestTempC && sp <= maxNestTempC) {
        applyExternalSetpoint(sp, "API");
        changed = true;
      }
    }

    // ✅ steps_per_degree (microsteps/°C)
    if (doc.containsKey("steps_per_degree")) {
      float newSteps = doc["steps_per_degree"];
      if (newSteps >= 10.0f && newSteps <= 2000.0f) {
        // store in full-step basis
        stepsPerDegreeBase = newSteps * microstepFactor;
        prefs.putFloat("stapPerGraad", stepsPerDegreeBase);

        // recalc runtime microstep value
        stepsPerDegreeCalc = stepsPerDegreeBase / microstepFactor;
        stepsPerDegreeNest = stepsPerDegreeCalc;

        changed = true;
      }
    }

    if (doc.containsKey("offset")) {
      float off = doc["offset"];
      if (off >= -10.0f && off <= 10.0f) {
        temperatureOffset = off;
        prefs.putFloat("tempOffset", temperatureOffset);
        changed = true;
      }
    }

    if (doc.containsKey("stepSize")) {
      float ss = doc["stepSize"];
      for (int i = 0; i < STEP_SIZE_COUNT; i++) {
        if (fabs(stepSizeOptions[i] - ss) < 0.001f) {
          stepSize = ss;
          stepSizeIndex = i;
          prefs.putFloat("stepSize", stepSize);
          changed = true;
          break;
        }
      }
    }

    StaticJsonDocument<512> resp;
    resp["success"] = changed;
    resp["setpoint"] = Setpoint;
    resp["temperature"] = lastMeasuredTemp;
    resp["offset"] = temperatureOffset;
    resp["stepSize"] = stepSize;
    resp["steps_per_degree"] = stepsPerDegreeNest;
    resp["steps_per_degree_fullstep"] = stepsPerDegreeBase;
    resp["domoticz_out_mode"] = cfg.domoticzOutMode;

    String response;
    serializeJson(resp, response);
    server.send(200, "application/json", response);
    return;
  }

  // ---------- Other methods ----------
  server.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
}
