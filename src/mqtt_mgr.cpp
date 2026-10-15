#include "mqtt_mgr.h"

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>

#include "config.h"
#include "rs485.h"
#include "shelly.h"
#include "telnet_log.h"

// PubSubClient braucht eine TCP-Verbindung (WiFiClient), über die es dann das
// MQTT-Protokoll spricht. Beide werden nur einmal angelegt und danach in
// diesem File wiederverwendet (deshalb "static", d.h. nur hier sichtbar).
static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

static unsigned long lastReconnectAttemptMillis = 0;
static const unsigned long RECONNECT_INTERVAL_MS = 5000;

static unsigned long lastPublishMillis = 0;
static const unsigned long PUBLISH_INTERVAL_MS = 3000;

// Wird von der PubSubClient-Bibliothek automatisch aufgerufen, sobald auf dem
// abonnierten Topic (config.mqtt_sub_topic) eine neue Nachricht ankommt. Man
// registriert diese Funktion einmal per setCallback() (siehe mqttBegin unten)
// und ruft sie selbst nie direkt auf.
// Der Callback läuft synchron in mqttClient.loop(), nicht im TCP-Callback.
static void mqttCallback(char *topic, byte *payload, unsigned int length) {
    (void)topic; // Topic wird ignoriert, wir haben nur ein einziges Sub-Topic abonniert

    if (config.mode != MODE_MQTT_SUB) return;

    float val;
    if (config.mqtt_input_format == MQTT_INPUT_SHELLY_JSON) {
        JsonDocument filter;
        filter["total_act_power"] = true;
        JsonDocument doc;
        const DeserializationError err = deserializeJson(
            doc, reinterpret_cast<const char *>(payload), length,
            DeserializationOption::Filter(filter));
        if (err) {
            LOG(("MQTT: ungültiges Shelly-JSON: " + String(err.c_str())).c_str());
            return;
        }
        JsonVariantConst total = doc["total_act_power"];
        if (!total.is<float>()) {
            LOG("MQTT: total_act_power fehlt oder ist keine Zahl");
            return;
        }
        val = total.as<float>();
    } else {
        char buf[32];
        if (length == 0 || length >= sizeof(buf) ||
            memchr(payload, '\0', length) != nullptr) {
            LOG("MQTT: ungültige Länge oder Nullbyte im Zahlenwert");
            return;
        }
        memcpy(buf, payload, length);
        buf[length] = '\0';
        char *end;
        val = strtof(buf, &end);
        if (end == buf) {
            LOG("MQTT: Messwert ist keine Zahl");
            return;
        }
        while (isspace(static_cast<unsigned char>(*end))) end++;
        if (*end != '\0') {
            LOG("MQTT: unerwarteter Inhalt nach dem Zahlenwert");
            return;
        }
    }
    if (!isfinite(val)) {
        LOG("MQTT: Messwert ist nicht endlich");
        return;
    }
    applyMeasurement(val);
    LOG(("MQTT: Messwert empfangen " + String(val)).c_str());
}

// Meldet einen Sensor bei Home Assistant an ("MQTT Discovery"). HA durchsucht
// dafür beim Start das Topic homeassistant/.../config nach solchen Nachrichten
// und legt daraus automatisch eine Entität an -- man muss den Sensor also NICHT
// mehr manuell in HA einrichten.
//
// Die Feldnamen (stat_t, val_tpl, uniq_id, pl_on/pl_off, dev_cla) sind keine
// eigene Abkürzung, sondern exakt die Kurzform, die Home Assistant für diese
// Nachricht erwartet (siehe HA-Doku "MQTT Discovery"). Es gibt auch die langen
// Namen (state_topic, value_template, ...), die funktionieren genauso, die
// Kurzform spart nur Bytes im ohnehin knappen ESP8266-RAM.
static void publishDiscoverySensor(const char *objectId, const char *name, const char *unit,
                                    const char *valueTemplate, const char *deviceClass,
                                    bool isBinary) {
    JsonDocument doc;
    doc["name"] = name;
    doc["uniq_id"] = String("soyo_") + objectId;     // eindeutige ID der Entität in HA
    doc["stat_t"] = config.mqtt_pub_topic;            // state_topic: wo HA den Wert abliest
    doc["val_tpl"] = valueTemplate;                   // value_template: Jinja-Ausdruck, der den Wert aus dem JSON-Payload holt
    if (unit != nullptr) doc["unit_of_meas"] = unit;  // unit_of_measurement, z.B. "W"
    if (deviceClass != nullptr) doc["dev_cla"] = deviceClass; // device_class: sagt HA, wie der Wert angezeigt werden soll (z.B. Signalstärke)
    if (isBinary) {
        // binary_sensor kennt nur zwei Zustände, dafür legt man den "Ein"/"Aus"-Text fest
        doc["pl_on"] = "ON";
        doc["pl_off"] = "OFF";
    }

    // "dev": fasst alle Sensoren zu einem gemeinsamen Gerät in HA zusammen,
    // statt dass jeder Sensor als eigenständiges Gerät auftaucht.
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"] = "soyo_controller";
    dev["name"] = "Soyosource Controller";
    dev["mf"] = "DIY";
    dev["mdl"] = "GTN1000/1200";

    String payload;
    serializeJson(doc, payload);

    // HA erwartet Discovery-Nachrichten unter diesem festen Topic-Schema:
    // homeassistant/<sensor|binary_sensor>/<beliebige_geräte-id>/<objekt-id>/config
    String topic = String("homeassistant/") + (isBinary ? "binary_sensor" : "sensor") +
                    "/soyo/" + objectId + "/config";
    // "true" am Ende = retained: die Nachricht bleibt beim Broker gespeichert,
    // damit HA sie auch findet, wenn es erst nach diesem Aufruf startet.
    mqttClient.publish(topic.c_str(), payload.c_str(), true);
}

static void publishDiscovery() {
    publishDiscoverySensor("demand", "Soyo Demand gesamt", "W", "{{ value_json.demand }}", nullptr, false);
    publishDiscoverySensor("demand_per_inverter", "Soyo Demand pro Inverter", "W", "{{ value_json.demand_per_inverter }}", nullptr, false);
    publishDiscoverySensor("netz", "Soyo Netz", "W", "{{ value_json.netz }}", nullptr, false);
    publishDiscoverySensor("verbrauch_l1", "Verbrauchszähler L1", "W", "{{ value_json.verbrauch_l1 }}", nullptr, false);
    publishDiscoverySensor("verbrauch_l2", "Verbrauchszähler L2", "W", "{{ value_json.verbrauch_l2 }}", nullptr, false);
    publishDiscoverySensor("verbrauch_l3", "Verbrauchszähler L3", "W", "{{ value_json.verbrauch_l3 }}", nullptr, false);
    publishDiscoverySensor("einspeisung", "Einspeisezähler gesamt", "W", "{{ value_json.einspeisung }}", nullptr, false);
    publishDiscoverySensor("einspeisung_l1", "Einspeisezähler L1", "W", "{{ value_json.einspeisung_l1 }}", nullptr, false);
    publishDiscoverySensor("einspeisung_l2", "Einspeisezähler L2", "W", "{{ value_json.einspeisung_l2 }}", nullptr, false);
    publishDiscoverySensor("einspeisung_l3", "Einspeisezähler L3", "W", "{{ value_json.einspeisung_l3 }}", nullptr, false);
    publishDiscoverySensor("rssi", "Soyo RSSI", "dBm", "{{ value_json.rssi }}", "signal_strength", false);
    publishDiscoverySensor("uptime", "Soyo Uptime", "s", "{{ value_json.uptime }}", nullptr, false);
    publishDiscoverySensor("heap", "Soyo Heap", "B", "{{ value_json.heap }}", nullptr, false);
    publishDiscoverySensor("notaus", "Soyo Notaus", nullptr,
                            "{{ 'ON' if value_json.notaus else 'OFF' }}", nullptr, true);
    publishDiscoverySensor("fallback", "Soyo Fallback", nullptr,
                            "{{ 'ON' if value_json.fallback else 'OFF' }}", nullptr, true);
    LOG("MQTT: Home Assistant Discovery gesendet");
}

static void attemptConnect() {
    // Jedes Gerät braucht am MQTT-Broker eine eindeutige Client-ID, sonst wirft
    // ein zweites Gerät mit derselben ID das erste laufend aus der Verbindung.
    // getChipId() ist pro ESP8266 einzigartig, deshalb eignet es sich dafür gut.
    String clientId = "soyo-" + String(ESP.getChipId(), HEX);
    bool ok;
    if (strlen(config.mqtt_user) > 0) {
        ok = mqttClient.connect(clientId.c_str(), config.mqtt_user, config.mqtt_pass);
    } else {
        ok = mqttClient.connect(clientId.c_str());
    }

    if (ok) {
        LOG("MQTT: verbunden");
        mqttClient.subscribe(config.mqtt_sub_topic);
        publishDiscovery();
    } else {
        LOG(("MQTT: Verbindung fehlgeschlagen, rc=" + String(mqttClient.state())).c_str());
    }
}

static void publishPeriodic() {
    unsigned long now = millis();
    if (now - lastPublishMillis < PUBLISH_INTERVAL_MS) return;
    lastPublishMillis = now;

    JsonDocument doc;
    doc["demand"] = g_demand * config.soyo_count;
    doc["demand_per_inverter"] = g_demand;
    doc["netz"] = g_netzwert;
    if (g_shellyPhaseValid[0]) doc["verbrauch_l1"] = g_shellyPhasePower[0]; else doc["verbrauch_l1"] = nullptr;
    if (g_shellyPhaseValid[1]) doc["verbrauch_l2"] = g_shellyPhasePower[1]; else doc["verbrauch_l2"] = nullptr;
    if (g_shellyPhaseValid[2]) doc["verbrauch_l3"] = g_shellyPhasePower[2]; else doc["verbrauch_l3"] = nullptr;
    if (g_shellyExportValid) doc["einspeisung"] = g_shellyExportTotalW; else doc["einspeisung"] = nullptr;
    if (g_shellyExportPhaseValid[0]) doc["einspeisung_l1"] = g_shellyExportPhasePower[0]; else doc["einspeisung_l1"] = nullptr;
    if (g_shellyExportPhaseValid[1]) doc["einspeisung_l2"] = g_shellyExportPhasePower[1]; else doc["einspeisung_l2"] = nullptr;
    if (g_shellyExportPhaseValid[2]) doc["einspeisung_l3"] = g_shellyExportPhasePower[2]; else doc["einspeisung_l3"] = nullptr;
    doc["einspeisung_stale"] = g_shellyExportStale;
    if (g_shellyExportValid) doc["einspeisung_age"] = (millis() - g_lastExportMeasurementMillis) / 1000;
    else doc["einspeisung_age"] = nullptr;
    doc["rssi"] = WiFi.RSSI();
    doc["mode"] = config.mode;
    doc["notaus"] = g_notaus;
    doc["fallback"] = g_fallbackActive;
    doc["uptime"] = millis() / 1000;
    doc["heap"] = ESP.getFreeHeap();

    String payload;
    serializeJson(doc, payload);
    if (!mqttClient.publish(config.mqtt_pub_topic, payload.c_str())) {
        LOG("MQTT: Status konnte nicht veröffentlicht werden");
    }
}

void mqttBegin() {
    if (!config.mqtt_enabled) return;
    if (!mqttClient.setBufferSize(768)) {
        LOG("MQTT: Statuspuffer konnte nicht vergrößert werden");
    }
    mqttClient.setServer(config.mqtt_broker, config.mqtt_port);
    mqttClient.setCallback(mqttCallback);

    // PubSubClient blockiert bei jedem connect()/publish()/loop() bis zu
    // MQTT_SOCKET_TIMEOUT lang (Standard: 15 Sekunden!), falls der Broker
    // nicht (mehr) antwortet -- z.B. bei einem kurzen Router-/Broker-Hänger.
    // In diesen 15 Sekunden läuft die komplette restliche Firmware nicht
    // weiter, insbesondere sendet rs485Loop() in dieser Zeit keinen neuen
    // Sollwert an den Soyo -- und der schaltet laut eigenem Protokoll bei
    // ausbleibenden RS485-Frames selbstständig auf 0W (siehe Kommentar bei
    // applyDemand() in rs485.cpp). Das reicht als Erklärung für beobachtete,
    // mehrere Sekunden lange Einspeise-Aussetzer bei aktivem MQTT. 2 Sekunden
    // reichen für einen normalen Broker völlig aus und begrenzen den
    // Blockier-Fall auf einen Bruchteil der bisherigen Zeit.
    mqttClient.setSocketTimeout(2);
    wifiClient.setTimeout(2000);
}

void mqttLoop() {
    if (!config.mqtt_enabled) return;
    if (WiFi.status() != WL_CONNECTED) return;

    if (!mqttClient.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectAttemptMillis >= RECONNECT_INTERVAL_MS) {
            lastReconnectAttemptMillis = now;
            attemptConnect();
        }
        return;
    }

    mqttClient.loop();
    publishPeriodic();
}

bool mqttIsConnected() {
    return mqttClient.connected();
}
