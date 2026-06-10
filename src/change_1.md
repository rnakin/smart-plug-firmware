# Firmware Change List — change_1

## 1. Status publish interval
**File:** `main.cpp`  
**What:** `STATUS_INTERVAL_MS` is 1000ms — publishes every second unconditionally.  
**Change:** Increase to 30000ms (30s), or publish only when relay state or device state changes.

```cpp
// Before
#define STATUS_INTERVAL_MS  1000

// After
#define STATUS_INTERVAL_MS  30000
```

---

## 2. Heartbeat publishes fake NFC scan event
**File:** `main.cpp` — inside `loop()` heartbeat block  
**What:** Every 60s, heartbeat calls `publishEvent("nfc_scan", ...)` — backend will treat this as a real card scan.  
**Change:** Replace with a dedicated `heartbeat` event type.

```cpp
// Before
publishEvent("nfc_scan", lastConfirmedUid.c_str());

// After
publishEvent("heartbeat", lastConfirmedUid.c_str());
```

---

## 3. Energy published in two places
**File:** `main.cpp`  
**What:** `readEnergy()` already handles threshold-based publishing every 1s. The 60s heartbeat block force-publishes energy again — duplicate logic.  
**Change:** Remove the energy publish block from the heartbeat. Trust `readEnergy()` threshold logic. Keep the 60s heartbeat only for the heartbeat event.

```cpp
// Remove from heartbeat block:
float v = pzem.voltage();
if (!isnan(v)) { ... publishEnergy(...); }
else { publishEnergyError(); }
```

---

## 4. readEnergy() runs while MQTT is disconnected
**File:** `main.cpp` — bottom of `loop()`  
**What:** `readEnergy()` calls `publishEnergy()` even when MQTT is disconnected — publish silently fails and readings are lost.  
**Change:** Gate `readEnergy()` on MQTT connection.

```cpp
// Before
if (now - lastEnergyReadMs >= 1000) {
    lastEnergyReadMs = now;
    readEnergy();
}

// After
if (mqttClient.connected() && now - lastEnergyReadMs >= 1000) {
    lastEnergyReadMs = now;
    readEnergy();
}
```

---

## 5. State not updated on MQTT disconnect
**File:** `main.cpp` — inside `loop()` WiFi-connected block  
**What:** If MQTT drops while in `ONLINE`, `state` stays `ONLINE` even though the client is disconnected. State and connection are out of sync.  
**Change:** Set `state = MQTT_CONNECTING` when MQTT connection is lost.

```cpp
// Add inside the WiFi-connected block:
if (!mqttClient.connected() && state == ONLINE) {
    state = MQTT_CONNECTING;
    Serial.println("[State] MQTT lost → MQTT_CONNECTING");
}
```

---

## Summary Table

| # | Issue | Impact | Change |
|---|---|---|---|
| 1 | Status every 1s | Broker flooded with redundant data | 30s interval or change-only publish |
| 2 | Heartbeat fakes NFC scan | Backend misreads heartbeat as card scan | Use `heartbeat` event type |
| 3 | Energy published twice | Duplicate data, redundant logic | Remove energy from heartbeat block |
| 4 | Energy reads while offline | Silent publish failures, lost readings | Gate on `mqttClient.connected()` |
| 5 | State/MQTT out of sync | State machine unreliable on reconnect | Set `MQTT_CONNECTING` on disconnect |

---

## 6. Serial baud rate — CLAUDE.md spec outdated
**File:** `CLAUDE.md`  
**What:** CLAUDE.md spec says 115200, but code and platformio.ini intentionally use 9600.  
**Change:** Update CLAUDE.md to match: `monitor_speed = 9600`, `--baud 9600`. No code change needed.

---

## 7. NFC null UID not published in JSON
**File:** `main.cpp` — `readNfc()` and `publishEvent()`  
**What:** When NFC tag is absent, `publishEvent("nfc_scan", nullptr)` skips the `uid` key entirely. CLAUDE.md spec requires `"uid": null` explicitly (vacancy detection is a feature).  
**Change:** Add `alwaysIncludeUid` parameter to `publishEvent()`, call with `true` for nfc_scan null case.

```cpp
// Before
void publishEvent(const char* type, const char* uid = nullptr) {
  ...
  if (uid) doc["uid"] = uid;
  ...
}

// After
void publishEvent(const char* type, const char* uid = nullptr, bool alwaysIncludeUid = false) {
  ...
  if (alwaysIncludeUid) {
    doc["uid"] = uid;
  } else if (uid) {
    doc["uid"] = uid;
  }
  ...
}
```

Call site in `readNfc()`:
```cpp
// Before
publishEvent("nfc_scan", nullptr);

// After
publishEvent("nfc_scan", nullptr, true);
```

---

## 8. Energy not published on heartbeat
**File:** `main.cpp` — `loop()` heartbeat block  
**What:** CLAUDE.md Energy Monitoring says "publish on 1% change in any field, or 1min heartbeat". The heartbeat block only publishes an event — it never publishes `energy_usage`. If power draw is stable, energy data is never sent.  
**Change:** After the heartbeat event, also publish energy if relay is ON.

```cpp
// Add inside the heartbeat block, after publishEvent("heartbeat", ...):
if (relayState) {
    float v = pzem.voltage();
    if (!isnan(v)) {
        float a = pzem.current();
        float w = pzem.power();
        float kwh = pzem.energy();
        lastPublishedWatts = w;
        lastPublishedVolts = v;
        lastPublishedAmps = a;
        lastPublishedKwh = kwh;
        publishEnergy(w, v, a, kwh);
    } else {
        publishEnergyError();
    }
}
```

---

## 9. Button not functional while WiFi/MQTT disconnected
**File:** `main.cpp` — `handleButton()`  
**What:** Button toggle only works when `state == ONLINE || state == ALERT`. When WiFi/MQTT drops, state becomes `MQTT_CONNECTING`, so the button does nothing. CLAUDE.md says "Relay and button remain functional while disconnected".  
**Change:** Add `MQTT_CONNECTING` to the allowed states.

```cpp
// Before
} else if (state == ONLINE || state == ALERT) {

// After
} else if (state == ONLINE || state == ALERT || state == MQTT_CONNECTING) {
```

---

## 10. Duplicate energy block runs every loop iteration
**File:** `main.cpp` — `loop()` inside `if (state == ONLINE)` block  
**What:** Lines 557-571 read and publish PZEM data on **every loop iteration** with no timer or change-detection, duplicating the proper `readEnergy()` function (which has threshold logic and runs every 1s). This floods MQTT.  
**Change:** Remove the entire block. Trust `readEnergy()` at the bottom of `loop()`.

```cpp
// Remove these lines from inside if (state == ONLINE):
          if (relayState) {
            float v = pzem.voltage();
            if (!isnan(v)) {
              float a = pzem.current();
              float w = pzem.power();
              float kwh = pzem.energy();
              lastPublishedWatts = w;
              lastPublishedVolts = v;
              lastPublishedAmps = a;
              lastPublishedKwh = kwh;
              publishEnergy(w, v, a, kwh);
            } else {
              publishEnergyError();
            }
          }
```

---

## Updated Summary Table

| # | Issue | Impact | Change |
|---|---|---|---|
| 1 | Status every 1s | Broker flooded with redundant data | 30s interval or change-only publish |
| 2 | Heartbeat fakes NFC scan | Backend misreads heartbeat as card scan | Use `heartbeat` event type |
| 3 | Energy published twice | Duplicate data, redundant logic | Remove energy from heartbeat block |
| 4 | Energy reads while offline | Silent publish failures, lost readings | Gate on `mqttClient.connected()` |
| 5 | State/MQTT out of sync | State machine unreliable on reconnect | Set `MQTT_CONNECTING` on disconnect |
| 6 | Serial baud 9600 vs 115200 | CLAUDE.md spec outdated | Update CLAUDE.md to 9600 |
| 7 | NFC null UID omitted | Vacancy detection broken for backend | Include explicit `"uid": null` |
| 8 | No energy heartbeat | Stable power never re-published | Publish energy on 1min heartbeat |
| 9 | Button dead when offline | Physical control lost on disconnect | Allow toggle in MQTT_CONNECTING |
| 10 | Energy every loop iteration | MQTT flooding, duplicate of readEnergy() | Remove duplicate block |
