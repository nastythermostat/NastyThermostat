# NastyThermostat

**Mechanically control a Nest Gen2 thermostat — fully local, no cloud, no API.**

A Nest Gen2 thermostat that lost Google connectivity? NastyThermostat gives it a second life by physically rotating the dial using a stepper motor and GT2 belt — controlled by an ESP32-C6 with MQTT, Domoticz, and Home Assistant integration.

![NastyThermostat hardware overview](img/photo.jpg)

[![NastyThermostat — Nest offline, fixed with gears and a motor](https://img.youtube.com/vi/sp1l5ZTAiks/maxresdefault.jpg)](https://youtu.be/sp1l5ZTAiks)

---

## How it works

```
ESP32-C6  →  DRV8825 driver  →  NEMA17 stepper  →  GT2 belt  →  Nest dial
```

The ESP32-C6 reads a DS18B20 temperature sensor and receives setpoints via rotary encoder, MQTT, or HTTP API. It calculates how many steps to move and physically rotates the Nest dial to the target temperature.

---

## Quick start

| Step | Guide |
|---|---|
| **Assemble the hardware** | [→ Build guide](https://nastythermostat.cc/build) |
| **Print the 3D parts** | [→ 3D models](https://nastythermostat.cc/3d-models) |
| **Flash the firmware** | [→ Webflash (browser, no software needed)](https://nastythermostat.cc/webflash) |
| **Configure WiFi, MQTT & settings** | [→ Setup guide](https://nastythermostat.cc/setup) |
| **See it in action** | [→ YouTube video](https://youtu.be/sp1l5ZTAiks) |

---

## Features

- DS18B20 local temperature sensor
- Rotary encoder for manual control
- ST7789 TFT display (1.47")
- MQTT publish/subscribe (temperature + setpoint)
- Domoticz integration (IDX based)
- Home Assistant MQTT Discovery (automatic `climate` entity)
- HTTP REST API
- WiFiManager configuration portal (no code changes needed)
- Persistent settings (ESP32 Preferences)
- Stepper power management (auto-disable when idle)

---

## Parts list

**[→ Download full parts list with supplier links (Excel)](https://github.com/nastythermostat/NastyThermostat/raw/main/docs/Part_List.xlsx)**  
**[→ Build guide — wiring and assembly details](https://nastythermostat.cc/build)**

| # | Component | Qty | ~Price |
|---|---|---|---|
| 1 | ESP32-C6 dev board with 1.47" ST7789 display | 1 | €15.89 |
| 2 | DRV8825 stepper driver module | 1 | €1.69 |
| 3 | Stepper driver expansion board (4-channel) | 1 | €1.01 |
| 4 | NEMA17 stepper motor (1.8°, 17 Ncm, 1A, 23mm) | 1 | €9.76 |
| 5 | DS18B20 temperature sensor module | 1 | €1.83 |
| 6 | KY-040 rotary encoder | 1 | €1.55 |
| 7 | GT2 closed loop belt 320mm | 1 | €1.89 |
| 8 | GT2 pulley 20T, 5mm bore | 1 | €1.00 |
| 9 | 12V 3A power adapter (5.5×2.1mm) | 1 | €11.68 |
| 10 | DC-DC buck converter 12V → 5V 5A | 1 | €1.89 |
| 11 | USB-C 90° adapter | 1 | €2.25 |
| 12–15 | Cables (power, Dupont, stepper, USB-C/Micro-USB) | — | €6.79 |
| 16–18 | M3×8 screws, M3 washers, M2×8 screws | — | — |
| 19–21 | 3D printed: Nest pulley (138T), mounting plate, stand | — | — |

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

**[→ 3D models page with previews and download](https://nastythermostat.cc/3d-models)**

STL files are in the [`/3d-models/`](3d-models/) folder. Print in PLA or PETG, 30% infill minimum.

| Part | Description |
|---|---|
| Nest pulley 138T | Attaches directly to the Nest dial ring |
| Motor mount | Mounts the NEMA17 to the base plate |
| Mounting plate | Base for all electronics and the Nest |
| Stand | Tilts the plate to the right angle |

---

## Flashing the firmware

**[→ Flash NastyThermostat via browser (no software needed)](https://nastythermostat.cc/webflash)**  
**[→ Full setup instructions](https://nastythermostat.cc/setup)**

The easiest way is via the browser — no software needed. Or build and upload with PlatformIO:

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
SSID:  NastyThermostatXXXXXX
URL:   http://192.168.4.1
```

Connect to this AP and open the portal to configure all settings.

| Setting | Description |
|---|---|
| **Device name** | Used for MQTT topics and AP SSID. Default: `NastyThermostatXXXXXX` |
| **MQTT server** | IP or hostname of your broker. Leave empty to disable MQTT. |
| **MQTT port** | Default: `1883` |
| **Domoticz IDX temperature** | Virtual sensor IDX for temperature. `0` = disabled. |
| **Domoticz IDX setpoint** | Virtual sensor IDX for setpoint. `0` = disabled. |
| **Domoticz gateway OUT mode** | `index` (default) = `domoticz/out/{idx}`. `flat` = `domoticz/out`, filtered by IDX. |
| **Homing after receiving setpoint** | `on` = full homing before move (accurate). `off` = small jiggle (faster, less wear). |
| **Home Assistant MQTT Discovery** | `on` = auto-creates a climate entity in HA. |
| **HTTP API token** | Required for POST requests. Leave empty to disable writes. |
| **MQTT publish interval** | How often temperature is published (seconds). Default: `30`. |

**[→ Full setup instructions with screenshots](https://nastythermostat.cc/setup)**

Re-open the portal anytime: **Menu → 5. WiFi setup → Start WiFi portal**, or hold the encoder button for 5 seconds at boot.

---

## MQTT topics

Topics use the device name (configurable in portal). Default: `NastyThermostatXXXXXX`

| Topic | Direction | Description |
|---|---|---|
| `{deviceName}/out/temp` | Publish | Current temperature (°C) |
| `{deviceName}/out/setpoint` | Publish (retained) | Current setpoint (°C) |
| `{deviceName}/in/setpoint` | Subscribe | Set new setpoint (float, e.g. `21.5`) |
| `{deviceName}/out/availability` | Publish (retained) | `online` / `offline` (LWT) |
| `{deviceName}/out/action` | Publish (retained) | `heating` or `idle` |
| `{deviceName}/out/mode` | Publish (retained) | `heat` (fixed, for HA compatibility) |

### Domoticz

```
Subscribe: domoticz/out/{idxSetpoint}   (index mode, default)
           domoticz/out                  (flat mode, filtered by idx)
Publish:   domoticz/in
```

### Home Assistant

With MQTT Discovery enabled, a `climate` entity is automatically created — no manual YAML needed.

---

## HTTP API

The device hosts a REST API on port 80.

### GET /api

```bash
curl http://192.168.x.x/api
```

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

### POST /api

Requires token (set in WiFiManager portal).

```bash
curl -X POST http://192.168.x.x/api \
  -H "Content-Type: application/json" \
  -d '{"token":"yourtoken","setpoint":21.0}'
```

| Field | Type | Description |
|---|---|---|
| `token` | string | **Required**. API token from portal. |
| `setpoint` | float | New setpoint in °C (9.0–32.0) |
| `offset` | float | Temperature offset correction (−10.0 to 10.0) |
| `stepSize` | float | Encoder step size: `0.1`, `0.2`, `0.5` or `1.0` |
| `steps_per_degree` | float | Calibration: microsteps per °C (10–2000) |

---

## On-device menu

Press the encoder button to open the menu. Rotate to navigate, press to select.

| Item | Description |
|---|---|
| 1. Homing | Perform manual homing (moves to mechanical stop, resets position to 0) |
| 2. Temperature offset | Correct DS18B20 reading ±10°C, press to save |
| 3. Steps/degree | Calibration: microsteps per °C, press to save |
| 4. Step size | Encoder resolution: 0.1 / 0.2 / 0.5 / 1.0°C |
| 5. WiFi setup | Open WiFiManager portal (device reboots into AP mode) |
| 6. Status | Shows firmware version, IP address, MQTT status |
| 7. Back | Return to main screen |

---

## Display

The main screen shows:
- **Large center**: current setpoint (°C)
- **Bottom left**: measured temperature (°C)
- **Top right**: WiFi status icon — green = connected, red = disconnected
- **Background**: orange when heating (setpoint > temperature), black when idle

Screen auto-off after 15 seconds of inactivity. Encoder movement or motor activity wakes the display.

---

## Calibration

Default: **856 microsteps/°C** (= 26.75 full-steps, based on 138T/20T pulley ratio at 1/32 microstepping).

If your Nest doesn't hit the right temperatures, adjust via:
- On-device menu → **3. Steps/degree**
- HTTP API: `{"token":"...","steps_per_degree": 856.0}`

---

## Serial commands

Connect at **115200 baud**.

| Command | Description |
|---|---|
| `?` or `h` | Show status banner (IP, MQTT broker, topics) |
| `i` | Show WiFi and MQTT connection status |

---

## Troubleshooting

**Motor moves but Nest temperature is wrong**  
→ Adjust Steps/degree in menu or via API. Start with small increments.

**WiFi not connecting**  
→ Check Serial Monitor at 115200 baud. Re-open portal via menu → **5. WiFi setup**.

**MQTT not connecting**  
→ Verify broker IP and port in portal. Check Serial Monitor output.

**DS18B20 reads 0°C or wrong temperature**  
→ Check wiring on GPIO9. Use temperature offset in menu to correct.

**[→ Setup guide — more help and troubleshooting](https://nastythermostat.cc/setup)**

---

## License

MIT — see [LICENSE](LICENSE)
