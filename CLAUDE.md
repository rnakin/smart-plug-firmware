# KnowWatt Smart Plug Firmware

ESP32-based smart plug with NFC device identification, energy monitoring, and MQTT communication.

---

## Hardware

| Component | Detail |
|---|---|
| MCU | ESP32 DevKit V1 |
| NFC Reader | PN532 via I2C |
| Energy Meter | PZEM-004T v3 TTL |
| Relay | GPIO 26 (HIGH = ON) |
| Button | GPIO 32 (active-high, physical toggle) |
| LED | GPIO 2 (active-high) |

### Wiring

| Pin | GPIO | Notes |
|---|---|---|
| Relay signal | 26 | HIGH = relay ON |
| Button | 32 | active-high |
| LED | 2 | active-high |
| PN532 SDA | 21 | I2C |
| PN532 SCL | 22 | I2C |
| PN532 I2C address | 0x24 | default, unchanged |
| PZEM RX (ESP receives) | 17 | Serial2 RX2 |
| PZEM TX (ESP sends) | 16 | Serial2 TX2 |

---

## Platform

- **IDE**: PlatformIO
- **Framework**: Arduino
- **Board**: `esp32dev`
- **Single file**: `src/main.cpp`

### `platformio.ini`
```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
lib_deps =
    adafruit/Adafruit PN532
    mandulaj/PZEM-004T-v30
    bblanchon/ArduinoJson
    tzapu/WiFiManager
    knolleary/PubSubClient
```

### Build & Flash
```bash
pio run --target upload
pio device monitor --baud 115200
```

---

## Configuration Constants

All hardware pins, timing, and network config are `#define` constants at the top of `main.cpp`. Never hardcode magic numbers inline.

| Constant | Default | Description |
|---|---|---|
| `PLUG_CODE` | `"AAAAAA"` | Unique plug identifier, must match DB |
| `MQTT_CLIENT_ID` | `"nfc_smart_plug_AAAAAA"` | MQTT client ID |
| `MQTT_HOST` | `"192.168.1.117"` | Broker IP |
| `MQTT_PORT` | `1883` | Broker port |
| `PIN_RELAY` | `26` | Relay GPIO |
| `PIN_BUTTON` | `32` | Button GPIO |
| `PIN_LED` | `2` | LED GPIO |
| `PIN_NFC_SDA` | `21` | PN532 SDA |
| `PIN_NFC_SCL` | `22` | PN532 SCL |
| `PZEM_RX` | `17` | Serial2 RX |
| `PZEM_TX` | `16` | Serial2 TX |
| `STATUS_INTERVAL_MS` | `30000` | `/status` publish interval |
| `HEARTBEAT_INTERVAL_MS` | `60000` | NFC + energy heartbeat |
| `ENERGY_CHANGE_THRESHOLD` | `0.01` | 1% change triggers publish |
| `NFC_DEBOUNCE_READS` | `2` | Reads to confirm UID or null |
| `NFC_DEBOUNCE_DELAY_MS` | `500` | Delay between debounce reads |
| `NTP_SERVER` | `"pool.ntp.org"` | NTP server |
| `NTP_UTC_OFFSET_SEC` | `25200` | UTC+7 (Thailand) |

---

## MQTT Protocol

**Broker**: Local Mosquitto, anonymous, no TLS.

### Topics

| Topic | Direction | Retained |
|---|---|---|
| `AAAAAA/command` | Backend → Firmware | No |
| `AAAAAA/config` | Backend → Firmware | No |
| `status` | Firmware → Backend | Yes |
| `energy_usage` | Firmware → Backend | No |
| `event` | Firmware → Backend | No |

> `plug_id` is always included in every outbound payload.

### Last Will
Topic: `status`, retained, published by broker on disconnect:
```json
{"plug_id": "AAAAAA", "state": "offline", "relay": false}
```

---

## Payload Reference

### Firmware → Backend

**`status`** — published every 30s and on connect:
```json
{"plug_id": "AAAAAA", "state": "online", "relay": true, "uptime": 3600, "rssi": -65, "ip": "192.168.1.42"}
```

**`energy_usage`** — on 1% change or 1min heartbeat (only when relay ON):
```json
{"plug_id": "AAAAAA", "watts": 120.5, "volts": 219.8, "amps": 1.01, "kwh": 0.042, "timestamp": "2026-06-04T10:00:00"}
```
- kWh from PZEM built-in accumulator
- Timestamp: ISO-8601, UTC+7
- On PZEM read failure: publish `{"plug_id": "AAAAAA", "error": "pzem_read_fail"}`

**`event`** — all events and ACKs:
```json
{"plug_id": "AAAAAA", "type": "relay_on"}
{"plug_id": "AAAAAA", "type": "relay_off"}
{"plug_id": "AAAAAA", "type": "nfc_scan", "uid": "5A04885A094189"}
{"plug_id": "AAAAAA", "type": "nfc_scan", "uid": null}
{"plug_id": "AAAAAA", "type": "alert"}
{"plug_id": "AAAAAA", "type": "alert_off"}
{"plug_id": "AAAAAA", "type": "heartbeat", "uid": "5A04885A094189"}
{"plug_id": "AAAAAA", "type": "heartbeat", "uid": null}
{"plug_id": "AAAAAA", "type": "ack", "command": "turn_on"}
```

### Backend → Firmware

**`AAAAAA/command`**:
```json
{"command": "turn_on"}
{"command": "turn_off"}
{"command": "alert"}
{"command": "alert_off"}
```
Every command triggers an ACK event immediately.

**`AAAAAA/config`** — sent after NFC tag registered to device:
```json
{"uid": "5A04885A094189", "device_name": "Rice Cooker", "rated_watts": 500}
```

---

## NFC Behavior

- **Library**: Adafruit PN532, I2C mode, polled in main loop
- **UID format**: continuous uppercase hex string e.g. `5A04885A094189`
- **Debounce**: every read (UID or null) must be confirmed by 2 reads 500ms apart
- **Publish logic**:
  - Confirmed value ≠ last published value → publish immediately
  - Confirmed value = last published value → wait for 1min heartbeat
- **Heartbeat**: publish current UID (or null) every 1min regardless of change
- **Null**: tag absent = `"uid": null` — vacancy detection is a feature

---

## Energy Monitoring

- **Library**: mandulaj/PZEM-004T-v30, Hardware Serial2
- **Read interval**: every 1s
- **Publish logic**: publish on 1% change in kWh, or 1min heartbeat
- **Only publish when relay is ON**
- **kWh**: read directly from PZEM built-in accumulator
- **On failure**: skip read, publish error event, stay functional

---

## LED Patterns

| State | Pattern |
|---|---|
| WiFi connecting | 3 short blinks, pause, repeat |
| First connect (WiFi+MQTT OK) | 2 short blinks once, then solid ON |
| Online / idle | Solid ON |
| Command received | 2 short blinks once, back to solid ON |
| Alert (warning before auto-cutoff) | Alternating long blink and long pause, repeat |
| Fault (WiFi down / MQTT down / PZEM error / auto-cutoff block) | 3 short blinks, pause, repeat |

**Fault state** blocks relay operation. Cleared only by button press or power cycle (full restart).

---

## Button Behavior

- **ONLINE / ALERT / MQTT_CONNECTING**: toggles relay → publishes `relay_on` or `relay_off` event
- **FAULT_BLOCK**: triggers full software restart (`ESP.restart()`)

---

## Startup Behavior

- Relay always starts OFF
- Clean state, no NVS restore
- WiFiManager captive portal (AP: `KnowWatt_Plug`) for WiFi setup → MQTT connect → publish `status` online → begin loop

---

## WiFi / MQTT Reconnection

- Both reconnect in background, non-blocking
- Relay and button remain functional while disconnected
- Missed publishes are dropped, no queue
- LED shows fault pattern while disconnected

---

## State Machine Summary

```
BOOT → WIFI_CONNECTING → MQTT_CONNECTING → ONLINE
                                              ↓
                                    (WiFi/MQTT drops) → FAULT pattern, reconnect in bg
                                              ↓
                                    ONLINE ←──┘ on reconnect

ONLINE + command("alert") → ALERT (relay unchanged, alert blink)
ALERT  + command("alert_off") → ONLINE (solid ON)
ALERT  + command("turn_off") → FAULT_BLOCK (3 blink repeat, relay OFF, blocked)
FAULT_BLOCK + button → ESP.restart()
```

---

## Common Tasks

**Change PLUG_CODE for a new device**
Update `PLUG_CODE` and `MQTT_CLIENT_ID` constants at top of `main.cpp`.

**Add a new command**
Add a new `else if` branch in `handleCommand()` and publish ACK via `publishAck()`.

**Change LED patterns**
Modify `updateLed()` — each pattern is a self-contained non-blocking sequence using `millis()`.

**Swap NFC library**
Replace PN532 init and `readNfc()` function. Keep debounce logic untouched.

**Add a new sensor**
Add read logic in `loop()` following the same change-detection + heartbeat pattern used for PZEM.

**Add a new event type**
Add a new `publishEvent()` call with the new `type` string. No structural changes needed.

---

## Debugging

| Tool | Usage |
|---|---|
| Serial monitor | `pio device monitor --baud 115200` — all state changes logged |
| MQTT spy | Subscribe to `#` on broker to see all topics live |
| MQTT publish test | `mosquitto_pub -h <broker_ip> -t "AAAAAA/command" -m '{"command":"turn_on"}'` |
| PZEM check | Serial logs print raw readings every second |
| NFC check | Serial logs print every confirmed UID read and debounce result |
