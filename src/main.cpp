#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <PZEM004Tv30.h>
#include <ArduinoJson.h>
#include <time.h>

#define PLUG_CODE                "AAAAAA"
#define MQTT_CLIENT_ID           "nfc_smart_plug_AAAAAA"
#define MQTT_HOST   "192.168.1.117"
#define MQTT_PORT                1883

#define PIN_RELAY                26
#define PIN_BUTTON               32
#define PIN_LED                  2
#define PIN_NFC_SDA              21
#define PIN_NFC_SCL              22
#define PZEM_RX                  17
#define PZEM_TX                  16

#define STATUS_INTERVAL_MS       30000
#define HEARTBEAT_INTERVAL_MS    60000
#define ENERGY_CHANGE_THRESHOLD  0.01f
#define NFC_DEBOUNCE_READS       2
#define NFC_DEBOUNCE_DELAY_MS    500

#define NTP_SERVER               "pool.ntp.org"
#define NTP_UTC_OFFSET_SEC       25200

enum State {
    BOOT,
    WIFI_CONNECTING,
    MQTT_CONNECTING,
    ONLINE,
    ALERT,
    FAULT_BLOCK
};

State state = BOOT;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

Adafruit_PN532 nfc(255, 255, &Wire);

PZEM004Tv30 pzem(Serial2, PZEM_RX, PZEM_TX);

unsigned long lastStatusMs = 0;
unsigned long lastHeartbeatMs = 0;
unsigned long lastEnergyReadMs = 0;
unsigned long lastLedUpdateMs = 0;
unsigned long lastNfcReadMs = 0;
unsigned long bootTime = 0;

unsigned long ntpSyncStartMs = 0;
int ntpRetries = 0;
bool ntpSynced = false;
bool ntpSyncStarted = false;

String lastConfirmedUid = "";
String pendingUid = "";
int nfcConfirmCount = 0;

float lastPublishedWatts = 0;
float lastPublishedVolts = 0;
float lastPublishedAmps = 0;
float lastPublishedKwh = 0;
bool firstEnergyReading = true;

bool relayState = false;

enum LedPattern {
    LED_SOLID,
    LED_3_SHORT_REPEAT,
    LED_2_SHORT_ONCE,
    LED_ALT_LONG_REPEAT
};
LedPattern ledPattern = LED_SOLID;
unsigned long ledPatternStartMs = 0;
bool patternSolidOn = false;

bool firstConnectDone = false;

void handleCommand(String cmd);
void publishStatus();
void publishEnergy(float w, float v, float a, float kwh);
void publishEnergyError();
void publishEvent(const char* type, const char* uid = nullptr, bool alwaysIncludeUid = false);
void publishAck(const char* command);
void readNfc();
void readEnergy();
void handleButton();
void updateLed();
void setLedPattern(LedPattern pattern, bool solidOn = false);
void startNtpSync();
bool updateNtpSync();
String getIsoTimestamp();
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String topicStr = String(topic);
    String msg = "";
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

    Serial.printf("[MQTT] Received on %s: %s\n", topicStr.c_str(), msg.c_str());

    if (topicStr.endsWith("/command")) {
        StaticJsonDocument<128> doc;
        DeserializationError err = deserializeJson(doc, msg);
        if (err) { Serial.println("JSON parse error"); return; }
        const char* cmd = doc["command"];
        if (cmd) handleCommand(String(cmd));
    } else if (topicStr.endsWith("/config")) {
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, msg);
        if (err) { Serial.println("JSON parse error"); return; }
        const char* uid = doc["uid"];
        const char* name = doc["device_name"];
        float rated = doc["rated_watts"];
        Serial.printf("[Config] UID=%s, Name=%s, Rated=%.1fW\n",
                      uid ? uid : "null", name ? name : "null", rated);
    }
}

void startNtpSync() {
    if (ntpSyncStarted) return;
    ntpSyncStarted = true;
    ntpSynced = false;
    ntpRetries = 0;
    ntpSyncStartMs = millis();
    configTime(NTP_UTC_OFFSET_SEC, 0, NTP_SERVER);
    Serial.print("[NTP] Syncing...");
}

bool updateNtpSync() {
    if (ntpSynced) return true;
    if (!ntpSyncStarted) return false;
    if (millis() - ntpSyncStartMs < (unsigned long)(ntpRetries + 1) * 500) return false;
    ntpRetries++;
    time_t now = time(nullptr);
    if (now >= 100000) {
        ntpSynced = true;
        Serial.println(" OK");
        return true;
    }
    Serial.print(".");
    if (ntpRetries >= 20) {
        Serial.println(" FAILED");
        ntpSynced = true;
        return true;
    }
    return false;
}

void handleCommand(String cmd) {
    if (state == FAULT_BLOCK) return;

    if (cmd == "turn_on") {
        digitalWrite(PIN_RELAY, HIGH);
        relayState = true;
        publishEvent("relay_on");
        publishAck("turn_on");
        publishStatus();
        setLedPattern(LED_2_SHORT_ONCE, true);
    } else if (cmd == "turn_off") {
        if (state == ALERT) {
            digitalWrite(PIN_RELAY, LOW);
            relayState = false;
            state = FAULT_BLOCK;
            publishEvent("relay_off");
            publishEvent("alert_off");
            publishAck("turn_off");
            publishStatus();
            setLedPattern(LED_3_SHORT_REPEAT);
            Serial.println("[State] FAULT_BLOCK");
            return;
        }
        digitalWrite(PIN_RELAY, LOW);
        relayState = false;
        publishEvent("relay_off");
        publishAck("turn_off");
        publishStatus();
        setLedPattern(LED_2_SHORT_ONCE, true);
    } else if (cmd == "alert") {
        if (state == ONLINE) {
            state = ALERT;
            publishEvent("alert");
            publishAck("alert");
            publishStatus();
            setLedPattern(LED_ALT_LONG_REPEAT);
            Serial.println("[State] ALERT");
        }
    } else if (cmd == "alert_off") {
        if (state == ALERT) {
            state = ONLINE;
            publishEvent("alert_off");
            publishAck("alert_off");
            publishStatus();
            setLedPattern(LED_SOLID, true);
            Serial.println("[State] ONLINE");
        }
    }
}

void publishStatus() {
    StaticJsonDocument<256> doc;
    doc["plug_id"] = PLUG_CODE;
    doc["state"] = (state == FAULT_BLOCK) ? "fault" :
    (state == ALERT) ? "alert" :
    (state == ONLINE) ? "online" : "connecting";
    doc["relay"] = relayState;
    doc["uptime"] = (millis() - bootTime) / 1000;
    doc["rssi"] = WiFi.RSSI();
    doc["ip"] = WiFi.localIP().toString();

    char buf[256];
    serializeJson(doc, buf);
    mqttClient.publish("status", buf, true);
}

void publishEnergy(float w, float v, float a, float kwh) {
    StaticJsonDocument<256> doc;
    doc["plug_id"] = PLUG_CODE;
    doc["watts"] = w;
    doc["volts"] = v;
    doc["amps"] = a;
    doc["kwh"] = kwh;
    doc["timestamp"] = getIsoTimestamp();

    char buf[256];
    serializeJson(doc, buf);
    mqttClient.publish("energy_usage", buf, false);
}

void publishEnergyError() {
    StaticJsonDocument<64> doc;
    doc["plug_id"] = PLUG_CODE;
    doc["error"] = "pzem_read_fail";

    char buf[64];
    serializeJson(doc, buf);
    mqttClient.publish("energy_usage", buf, false);
}

void publishEvent(const char* type, const char* uid, bool alwaysIncludeUid) {
    StaticJsonDocument<128> doc;
    doc["plug_id"] = PLUG_CODE;
    doc["type"] = type;
    if (alwaysIncludeUid) {
        doc["uid"] = uid;
    } else if (uid) {
        doc["uid"] = uid;
    }

    char buf[128];
    serializeJson(doc, buf);
    mqttClient.publish("event", buf, false);
}

void publishAck(const char* command) {
    StaticJsonDocument<128> doc;
    doc["plug_id"] = PLUG_CODE;
    doc["type"] = "ack";
    doc["command"] = command;

    char buf[128];
    serializeJson(doc, buf);
    mqttClient.publish("event", buf, false);
}

void readNfc() {
    unsigned long now = millis();
    if (now - lastNfcReadMs < NFC_DEBOUNCE_DELAY_MS) return;
    lastNfcReadMs = now;

    uint8_t uid[7];
    uint8_t uidLen;
    bool found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50);

    String newUid = "";
    if (found && uidLen > 0) {
        for (uint8_t i = 0; i < uidLen; i++) {
            if (uid[i] < 0x10) newUid += "0";
            newUid += String(uid[i], HEX);
        }
        newUid.toUpperCase();
    }

    if (nfcConfirmCount == 0) {
        pendingUid = newUid;
        nfcConfirmCount = 1;
    } else {
        if (newUid == pendingUid) {
            nfcConfirmCount++;
            if (nfcConfirmCount >= NFC_DEBOUNCE_READS) {
                if (pendingUid != lastConfirmedUid) {
                    lastConfirmedUid = pendingUid;
                    if (pendingUid.isEmpty()) {
                        publishEvent("nfc_scan", nullptr, true);
                    } else {
                        publishEvent("nfc_scan", pendingUid.c_str());
                    }
                }
                nfcConfirmCount = 0;
                pendingUid = "";
            }
        } else {
            pendingUid = newUid;
            nfcConfirmCount = 1;
        }
    }
}

void readEnergy() {
    if (!relayState) return;

    float v = pzem.voltage();
    if (isnan(v)) {
        publishEnergyError();
        return;
    }

    float a = pzem.current();
    float w = pzem.power();
    float kwh = pzem.energy();

    if (firstEnergyReading) {
        firstEnergyReading = false;
        lastPublishedWatts = w;
        lastPublishedVolts = v;
        lastPublishedAmps = a;
        lastPublishedKwh = kwh;
        publishEnergy(w, v, a, kwh);
        return;
    }

    float kwhChange = fabs(kwh - lastPublishedKwh);

    if (kwhChange >= ENERGY_CHANGE_THRESHOLD) {
        lastPublishedWatts = w;
        lastPublishedVolts = v;
        lastPublishedAmps = a;
        lastPublishedKwh = kwh;
        publishEnergy(w, v, a, kwh);
    }
}

void handleButton() {
    static bool lastButtonState = LOW;
    static unsigned long lastDebounceMs = 0;
    static bool processed = false;

    bool reading = digitalRead(PIN_BUTTON);
    if (reading != lastButtonState) lastDebounceMs = millis();
    lastButtonState = reading;

    if (reading == HIGH && (millis() - lastDebounceMs) > 50) {
        if (!processed) {
            processed = true;
            if (state == FAULT_BLOCK) {
                Serial.println("[Button] Fault block - restarting");
                ESP.restart();
            } else if (state == ONLINE || state == ALERT || state == MQTT_CONNECTING) {
                relayState = !relayState;
                digitalWrite(PIN_RELAY, relayState ? HIGH : LOW);
                publishEvent(relayState ? "relay_on" : "relay_off");
            }
        }
    } else if (reading == LOW) {
        processed = false;
    }
}

void setLedPattern(LedPattern pattern, bool solidOn) {
    ledPattern = pattern;
    ledPatternStartMs = millis();
    patternSolidOn = solidOn;
    if (pattern == LED_SOLID) {
        digitalWrite(PIN_LED, solidOn ? HIGH : LOW);
    }
}

void updateLed() {
    unsigned long now = millis();
    unsigned long elapsed = now - ledPatternStartMs;

    switch (ledPattern) {
        case LED_SOLID:
            break;

        case LED_3_SHORT_REPEAT: {
            unsigned long cycle = elapsed % 1900;
            if (cycle < 150) {
                digitalWrite(PIN_LED, HIGH);
            } else if (cycle < 300) {
                digitalWrite(PIN_LED, LOW);
            } else if (cycle < 450) {
                digitalWrite(PIN_LED, HIGH);
            } else if (cycle < 600) {
                digitalWrite(PIN_LED, LOW);
            } else if (cycle < 750) {
                digitalWrite(PIN_LED, HIGH);
            } else if (cycle < 900) {
                digitalWrite(PIN_LED, LOW);
            } else {
                digitalWrite(PIN_LED, LOW);
            }
            break;
        }

        case LED_2_SHORT_ONCE: {
            if (elapsed < 150) {
                digitalWrite(PIN_LED, HIGH);
            } else if (elapsed < 300) {
                digitalWrite(PIN_LED, LOW);
            } else if (elapsed < 450) {
                digitalWrite(PIN_LED, HIGH);
            } else if (elapsed < 600) {
                digitalWrite(PIN_LED, LOW);
            } else {
                digitalWrite(PIN_LED, patternSolidOn ? HIGH : LOW);
                ledPattern = LED_SOLID;
            }
            break;
        }

        case LED_ALT_LONG_REPEAT: {
            unsigned long cycle = elapsed % 3000;
            digitalWrite(PIN_LED, cycle < 1500 ? HIGH : LOW);
            break;
        }
    }
}

String getIsoTimestamp() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", t);
    return String(buf);
}

void connectMqtt() {
    if (mqttClient.connected()) return;

    Serial.printf("[MQTT] Connecting to %s:%d...\n", MQTT_HOST, MQTT_PORT);

    StaticJsonDocument<64> willDoc;
    willDoc["plug_id"] = PLUG_CODE;
    willDoc["state"] = "offline";
    willDoc["relay"] = false;
    char willBuf[64];
    serializeJson(willDoc, willBuf);

    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);

    bool ok = mqttClient.connect(MQTT_CLIENT_ID, NULL, NULL,
                                 "status", 1, true, willBuf);
    if (ok) {
        Serial.printf("[MQTT] Connected as %s\n", MQTT_CLIENT_ID);
        String cmdTopic = String(PLUG_CODE) + "/command";
        String configTopic = String(PLUG_CODE) + "/config";
        mqttClient.subscribe(cmdTopic.c_str());
        mqttClient.subscribe(configTopic.c_str());
        publishStatus();

        if (!firstConnectDone) {
            firstConnectDone = true;
            setLedPattern(LED_2_SHORT_ONCE, true);
        }

        if (state == MQTT_CONNECTING) {
            state = ONLINE;
            Serial.println("[State] ONLINE");
            setLedPattern(LED_SOLID, true);
        }
    } else {
        Serial.printf("[MQTT] Failed, rc=%d\n", mqttClient.state());
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== KnowWatt Smart Plug ===");

    bootTime = millis();

    pinMode(PIN_RELAY, OUTPUT);
    pinMode(PIN_BUTTON, INPUT_PULLDOWN);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_RELAY, LOW);
    digitalWrite(PIN_LED, LOW);

    Wire.begin(PIN_NFC_SDA, PIN_NFC_SCL);
    nfc.begin();
    uint32_t versiondata = nfc.getFirmwareVersion();
    if (versiondata) {
        Serial.printf("[NFC] PN532 v%d.%d\n", (versiondata >> 16) & 0xFF, (versiondata >> 8) & 0xFF);
        nfc.SAMConfig();
    } else {
        Serial.println("[NFC] PN532 not found");
    }

    Serial.println(pzem.readAddress(), HEX);
    state = WIFI_CONNECTING;
    setLedPattern(LED_3_SHORT_REPEAT);

    Serial.printf("[WiFi] Connecting...\n");
    WiFi.mode(WIFI_STA);
    WiFiManager wm;
    if (!wm.autoConnect("KnowWatt_Plug")) {
        Serial.println("[WiFi] Failed to connect");
        ESP.restart();
    }
    Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
}

void loop() {
    unsigned long now = millis();
    static bool wifiWasDisconnected = true;
    static unsigned long lastWifiRetryMs = 0;
    bool wifiConnected = (WiFi.status() == WL_CONNECTED);

    if (wifiConnected) {
        if (wifiWasDisconnected) {
            wifiWasDisconnected = false;
            state = MQTT_CONNECTING;
            startNtpSync();
            setLedPattern(LED_SOLID, true);
        }
        if (!mqttClient.connected()) {
            if (updateNtpSync()) {
                connectMqtt();
            }
        } else {
            mqttClient.loop();

            if (!mqttClient.connected() && state == ONLINE) {
                state = MQTT_CONNECTING;
                Serial.println("[State] MQTT lost → MQTT_CONNECTING");
            }

            if (state == ONLINE) {
                if (now - lastStatusMs >= STATUS_INTERVAL_MS) {
                    lastStatusMs = now;
                    publishStatus();
                }

                if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;

    if (lastConfirmedUid.isEmpty()) {
        publishEvent("heartbeat", nullptr);
    } else {
        publishEvent("heartbeat", lastConfirmedUid.c_str());
    }

    // Always read PZEM regardless of relay state for self-check
    float v = pzem.voltage();
    if (!isnan(v)) {
        float a = pzem.current();
        float w = pzem.power();
        float kwh = pzem.energy();

        // Relay fault detection: if relay is OFF but current is flowing
        if (!relayState && a > 0.1f) {
            Serial.println("[FAULT] Current detected while relay is OFF!");
            publishEvent("relay_fault");
            state = FAULT_BLOCK;
            setLedPattern(LED_3_SHORT_REPEAT);
        }

        lastPublishedWatts = w;
        lastPublishedVolts = v;
        lastPublishedAmps = a;
        lastPublishedKwh = kwh;
        publishEnergy(w, v, a, kwh);
    } else {
        publishEnergyError();
        Serial.println("[PZEM] Read failed during heartbeat self-check");
    }
}
            }
        }
    } else {
        if (!wifiWasDisconnected) {
            wifiWasDisconnected = true;
            if (state != WIFI_CONNECTING && state != MQTT_CONNECTING && state != BOOT) {
                setLedPattern(LED_3_SHORT_REPEAT);
            }
        }
        if (now - lastWifiRetryMs > 10000) {
            lastWifiRetryMs = now;
            WiFi.reconnect();
        }
    }

    handleButton();
    updateLed();
    readNfc();

    if (mqttClient.connected() && now - lastEnergyReadMs >= 1000) {
        lastEnergyReadMs = now;
        readEnergy();
    }
}
