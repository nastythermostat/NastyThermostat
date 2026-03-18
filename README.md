# NastyThermostat

**Mechanically control a Nest Gen2 thermostat locally — no cloud, no API, full automation.**

A Nest Gen2 thermostat that lost Google connectivity? NastyThermostat gives it a second life by physically turning the dial using a stepper motor, GT2 belt and 3D-printed gear — controlled by an ESP32-C6 with full MQTT, Domoticz and Home Assistant integration.

![NastyThermostat hardware overview](img/photo.jpg)

> [Build guide](https://nastythermostat.github.io/NastyThermostat/build/)
> [3D-Models](https://nastythermostat.github.io/NastyThermostat/3d-Models/)
> [Setup](https://nastythermostat.github.io/NastyThermostat/setup/)
> [Webflash](https://nastythermostat.cc/webflash)

---

## Table of Contents

- [How it works](#how-it-works)
- [Features](#features)
- [Parts list](#parts-list)
- [Pin mapping](#pin-mapping)
- [3D printed parts](#3d-printed-parts)
- [Installation](#installation)
- [Configuration](#configuration)
- [MQTT topics](#mqtt-topics)
- [HTTP API](#http-api)
- [On-device menu](#on-device-menu)
- [Display](#display)
- [Calibration](#calibration)
- [Serial commands](#serial-commands)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## How it works

```
ESP32-C6  →  DRV8825 driver  →  NEMA17 stepper  →  GT2 belt  →  Nest dial
```

The ESP32-C6 reads a DS18B20 temperature sensor and receives setpoints via rotary encoder, MQTT or HTTP API. It then calculates how many steps to move and physically rotates the Nest dial to the target temperature.

---

## Features

- 🌡️ DS18B20 local temperature sensor
- 🎛️ Rotary encoder for manual control
- 📺 ST7789 TFT display (1.47")
- 📡 MQTT publish/subscribe (temperature + setpoint)
- 🏠 Domoticz integration (IDX based)
- 🤖 Home Assistant MQTT Discovery (climate entity)
- 🌐 HTTP REST API
- 🔧 WiFiManager configuration portal
- 💾 Persistent settings (ESP32 Preferences)
- ⚡ Stepper power management (auto-disable when idle)

---

## Parts list

> Full parts list also available as [Excel spreadsheet](docs/Part_List.xlsx)

| # | Category | Component | Description | Qty | ~Price |
|---|---|---|---|---|---|
| 1 | Electronics | ESP32-C6 LCD 1.47 | ESP32-C6 dev board with 1.47" ST7789 display | 1 | €15.89 |
| 2 | Electronics | DRV8825 Stepper Driver | Microstepping driver module (heatsink included) | 1 | €1.69 |
| 3 | Electronics | Stepper Driver Expansion Board | 4-channel driver carrier board | 1 | €1.01 |
| 4 | Electronics | NEMA17 Stepper Motor | 1.8°, 17 Ncm, 1A, 23mm body | 1 | €9.76 |
| 5 | Electronics | DS18B20 Temperature Sensor | Digital 1-Wire temperature module | 1 | €1.83 |
| 6 | Electronics | KY-040 Rotary Encoder | 360° encoder with push button | 1 | €1.55 |
| 7 | Mechanical | GT2 Belt 320mm | Closed loop 2GT-320-6mm belt | 1 | €1.89 |
| 8 | Mechanical | GT2 Pulley 20T | 20 teeth, 5mm bore, on stepper motor | 1 | €1.00 |
| 9 | Power | 12V 3A Power Adapter | 5.5x2.1mm DC output | 1 | €11.68 |
| 10 | Power | DC-DC Buck Converter | 12V to 5V 5A step-down module | 1 | €1.89 |
| 11 | Power | USB-C 90° Adapter | Powers the ESP32 | 1 | €2.25 |
| 12 | Wiring | 12V Power Cable | 14cm red/black, adapter → driver | 1 | €1.00 |
| 13 | Wiring | Dupont Jumper Wires | 10cm Female-to-Female pack | 1 | €1.50 |
| 14 | Wiring | USB-C / Micro-USB Split Cable | 0.2m, powers Nest + ESP32 | 1 | €3.29 |
| 15 | Wiring | Stepper Motor Cable | JST XH2.54mm female, 10-15cm | 1 | €1.00 |
| 16 | Hardware | M3×8 Screws | Mounting | 8 | — |
| 17 | Hardware | M3 Washers | Mounting | 6 | — |
| 18 | Hardware | M2×8 Screws | Mounting | 2 | — |
| 19 | 3D Print | GT2 Pulley 138T | Custom pulley for Nest thermostat | 1 | — |
| 20 | 3D Print | Mounting Plate | Baseplate for electronics and Nest | 1 | — |
| 21 | 3D Print | Stand | Stand for the mounting plate | 1 | — |

**Total electronics + mechanical: ~€55**

---

## Pin mapping



| Function | GPIO |
|---|---|
| TFT CS | 14 |
| TFT DC | 15 |
| TFT RST | 21 |
| TFT Backlight | 22 |
| TFT MOSI | 6 |
| TFT SCLK | 7 |
| Stepper STEP | 2 |
| Stepper DIR | 3 |
| Stepper ENABLE | 5 |
| Encoder CLK | 0 |
| Encoder DT | 1 |
| Encoder Button | 4 |
| DS18B20 | 9 |
![Wiring diagram](img/wiring.jpg)
---


## 3D printed parts

STL files are in the `/3d-models/` folder. Print the Nest pulley (138T) and motor mount in PLA or PETG, 30% infill minimum. The pulley attaches directly to the Nest dial ring.

---

## Installation

### 1. Flash the firmware

The easiest way is via the web flasher — no software needed:

👉 **[Flash NastyThermostat via browser](https://nastythermostat.github.io/NastyThermostat/webflash/)**

Or build yourself using PlatformIO:

```bash
git clone https://github.com/nastythermostat/NastyThermostat.git
cd NastyThermostat
cp include/SecretsTemplate.h include/arduino_secrets.h
pio run --target upload
```

---

## Configuration

### First boot — WiFiManager portal

On first boot (or when WiFi is not found), the device creates an access point:

```
SSID: NastyThermostat (or NastyThermostatXXXXXX with unique MAC suffix)
URL:  http://192.168.4.1
```

Connect to this AP and open the portal. All settings are configured here:

| Setting | Description |
|---|---|
| **Device name** | Unique name used for MQTT topics and AP SSID. Default: `NastyThermostatXXXXXX` |
| **MQTT server** | IP or hostname of your MQTT broker. Leave empty to disable MQTT. |
| **MQTT port** | Default: `1883` |
| **Domoticz IDX temperature** | Domoticz virtual sensor IDX for temperature. Leave `0` to disable. |
| **Domoticz IDX setpoint** | Domoticz virtual sensor IDX for setpoint. Leave `0` to disable. |
| **Domoticz gateway OUT mode** | `index` (default) = subscribe to `domoticz/out/{idx}`. `flat` = subscribe to `domoticz/out` and filter by IDX. |
| **Homing after receiving setpoint** | `on` (default): performs full homing before moving to new setpoint (most accurate). `off`: performs a small wake-jiggle instead (faster, less mechanical wear). |
| **Home Assistant MQTT Discovery** | `on` (default): automatically creates a climate entity in Home Assistant. |
| **HTTP API token** | Token required for POST requests to `/api`. Leave empty to disable writes. Type `clear` to remove an existing token. |
| **MQTT publish interval** | How often temperature is published in seconds. Default: `30`. |

### Re-opening the portal

From the on-device menu: **5. WiFi setup → Start WiFi portal (reboot)**

Or hold the encoder button for 5 seconds at boot.

---

## MQTT topics

Topics are based on the device name (configurable). Default: `NastyThermostatXXXXXX`

| Topic | Direction | Description |
|---|---|---|
| `{deviceName}/out/temp` | Publish | Current temperature (°C) |
| `{deviceName}/out/setpoint` | Publish (retained) | Current setpoint (°C) |
| `{deviceName}/in/setpoint` | Subscribe | Set new setpoint (plain float, e.g. `21.5`) |
| `{deviceName}/out/availability` | Publish (retained) | `online` / `offline` (LWT) |
| `{deviceName}/out/action` | Publish (retained) | `heating` or `idle` |
| `{deviceName}/out/mode` | Publish (retained) | `heat` (always, for HA) |

### Domoticz

NastyThermostat subscribes to Domoticz setpoint changes and publishes temperature and setpoint back to Domoticz using the configured IDX numbers.

```
Subscribe: domoticz/out/{idxSetpoint}   (index mode, default)
           domoticz/out                  (flat mode, filtered by idx)
Publish:   domoticz/in
```

### Home Assistant

With MQTT Discovery enabled, a `climate` entity is automatically created in HA showing current temperature, setpoint, and heating/idle action. No manual YAML needed.

---

## HTTP API

The device hosts a REST API on port 80.

### GET /api — Status

```bash
curl http://192.168.x.x/api
```

Response:
```json
{
  "device_name": "NastyThermostat123456",
  "setpoint": 20.5,
  "temperature": 19.8,
  "offset": 0.0,
  "stepSize": 0.1,
  "motor_position": 14420,
  "steps_per_degree": 856.0,
  "mqtt_enabled": true
}
```

### POST /api — Update settings

Requires token (set in WiFiManager portal).

```bash
curl -X POST http://192.168.x.x/api \
  -H "Content-Type: application/json" \
  -d '{"token":"yourtoken","setpoint":21.0}'
```

| Field | Type | Description |
|---|---|---|
| `token` | string | **Required**. API token set in portal. |
| `setpoint` | float | New setpoint in °C (9.0–32.0) |
| `offset` | float | Temperature offset correction (-10.0 to 10.0) |
| `stepSize` | float | Encoder step size: `0.1`, `0.2`, `0.5` or `1.0` |
| `steps_per_degree` | float | Calibration: microsteps per °C (10–2000) |

---

## On-device menu

Press the encoder button to open the menu. Rotate to navigate, press to select.

| Menu item | Description |
|---|---|
| 1. Homing | Perform a manual homing cycle (moves to mechanical stop, resets position to 0) |
| 2. Temperature offset | Correct DS18B20 reading. Turn to adjust ±10°C, press to save. |
| 3. Steps/degree | Calibration value in microsteps/°C. Turn to adjust, press to save. |
| 4. Step size | Encoder step size: 0.1 / 0.2 / 0.5 / 1.0°C |
| 5. WiFi setup | Open WiFiManager portal (device reboots into AP mode) |
| 6. Status | Shows firmware version, WiFi IP, MQTT connection status |
| 7. Back | Return to main screen |

---

## Display

The main screen shows:
- **Large center**: current setpoint (°C)
- **Bottom left**: measured temperature (°C)
- **Top right**: WiFi status icon (green = connected, red = disconnected)
- **Background**: orange when heating (setpoint > temperature), black when idle

Screen auto-off after 15 seconds of inactivity. Any encoder movement or motor activity wakes the display.

---

## Calibration

The default `steps_per_degree` value is **26.75 full-steps/°C** (= 856 microsteps at 1/32). This is based on the 138T/20T pulley ratio. If your Nest doesn't hit the right temperatures, adjust via:

- Menu item **3. Steps/degree**
- Or via HTTP API: `{"token":"...","steps_per_degree": 856.0}`

---

## Serial commands

Connect via Serial Monitor at 115200 baud:

| Command | Description |
|---|---|
| `?` or `h` | Show status banner (IP, MQTT, topics) |
| `i` | Show WiFi and MQTT connection status |

---

## Troubleshooting

**Motor moves but Nest temperature is wrong**
→ Adjust Steps/degree in menu or via API. Start with small increments.

**WiFi not connecting**
→ Check Serial Monitor output at 115200 baud. Verify status.

**MQTT not connecting**
→ Check Serial Monitor output at 115200 baud. Verify broker IP and port in portal.

**DS18B20 reads 0°C or wrong temperature**
→ Check wiring on GPIO9. Use temperature offset in menu to correct.

---

## License

MIT — see [LICENSE](LICENSE)
