#include "storage.h"

#include <LittleFS.h>
#include <ArduinoJson.h>

#include "telnet_log.h"

// LittleFS ist ein kleines Dateisystem, das im Flash-Speicher des ESP8266
// lebt -- es überlebt also einen Neustart oder Stromausfall, anders als
// normale Variablen im RAM. Die komplette Konfiguration wird hier als eine
// einzige JSON-Datei "/config.json" abgelegt, gelesen und geschrieben.
static const char *CONFIG_PATH = "/config.json";

void configSetDefaults(Config &c) {
    memset(&c, 0, sizeof(Config));

    c.wifi_static = false;
    strlcpy(c.wifi_mask, "255.255.255.0", sizeof(c.wifi_mask));
    c.wifi_11n = false;

    c.mqtt_enabled = false;
    c.mqtt_port = 1883;
    c.mqtt_input_format = MQTT_INPUT_NUMBER;
    strlcpy(c.mqtt_sub_topic, "Soyosource/L1L2L3", sizeof(c.mqtt_sub_topic));
    strlcpy(c.mqtt_pub_topic, "Soyosource/status", sizeof(c.mqtt_pub_topic));

    c.mode = MODE_SHELLY_GEN1;
    c.static_watt = 50;

    c.shelly_l1 = true;
    c.shelly_l2 = true;
    c.shelly_l3 = true;
    c.poll_interval_ms = 500;
    c.shelly_export_l1 = true;
    c.shelly_export_l2 = true;
    c.shelly_export_l3 = true;

    c.max_power = 900;
    c.soyo_count = 1;
    c.calibration_offset_w = 0;
    c.fallback_watt = 0;
    c.rs485_send_interval_ms = 1100;
    c.tolerance_import_w = 10;
    c.tolerance_export_w = 10;
    c.correction_gain_percent = 50;

    c.night_mode_enabled = false;
    c.night_start_h = 22;
    c.night_start_m = 0;
    c.night_end_h = 6;
    c.night_end_m = 0;
    c.night_max_power = 300;
}

// configToDoc() (Config -> JSON) und docToConfig() (JSON -> Config, weiter
// unten) sind die einzigen zwei Stellen, die wissen, wie ein Config-Feld auf
// einen JSON-Schlüssel abgebildet wird. Datei schreiben/lesen UND der
// Webinterface-Download/-Upload (siehe http_server.cpp) rufen beide nur diese
// zwei Funktionen auf -- ein neues Config-Feld muss also nur hier in diesen
// zwei Funktionen ergänzt werden, nicht zusätzlich in jeder aufrufenden Stelle.
static void configToDoc(const Config &c, JsonDocument &doc) {
    doc["wifi_ssid"] = c.wifi_ssid;
    doc["wifi_pass"] = c.wifi_pass;
    doc["wifi_ssid2"] = c.wifi_ssid2;
    doc["wifi_pass2"] = c.wifi_pass2;
    doc["wifi_static"] = c.wifi_static;
    doc["wifi_ip"] = c.wifi_ip;
    doc["wifi_gw"] = c.wifi_gw;
    doc["wifi_mask"] = c.wifi_mask;
    doc["wifi_11n"] = c.wifi_11n;

    doc["mqtt_enabled"] = c.mqtt_enabled;
    doc["mqtt_broker"] = c.mqtt_broker;
    doc["mqtt_port"] = c.mqtt_port;
    doc["mqtt_user"] = c.mqtt_user;
    doc["mqtt_pass"] = c.mqtt_pass;
    doc["mqtt_sub_topic"] = c.mqtt_sub_topic;
    doc["mqtt_input_format"] = c.mqtt_input_format;
    doc["mqtt_pub_topic"] = c.mqtt_pub_topic;

    doc["mode"] = c.mode;
    doc["static_watt"] = c.static_watt;

    doc["shelly_ip"] = c.shelly_ip;
    doc["shelly_l1"] = c.shelly_l1;
    doc["shelly_l2"] = c.shelly_l2;
    doc["shelly_l3"] = c.shelly_l3;
    doc["poll_interval_ms"] = c.poll_interval_ms;
    doc["shelly_export_ip"] = c.shelly_export_ip;
    doc["shelly_export_l1"] = c.shelly_export_l1;
    doc["shelly_export_l2"] = c.shelly_export_l2;
    doc["shelly_export_l3"] = c.shelly_export_l3;

    doc["json_url"] = c.json_url;
    doc["json_path"] = c.json_path;

    doc["max_power"] = c.max_power;
    doc["soyo_count"] = c.soyo_count;
    doc["calibration_offset_w"] = c.calibration_offset_w;
    doc["fallback_watt"] = c.fallback_watt;
    doc["rs485_send_interval_ms"] = c.rs485_send_interval_ms;
    doc["tolerance_import_w"] = c.tolerance_import_w;
    doc["tolerance_export_w"] = c.tolerance_export_w;
    doc["correction_gain_percent"] = c.correction_gain_percent;

    doc["night_mode_enabled"] = c.night_mode_enabled;
    doc["night_start_h"] = c.night_start_h;
    doc["night_start_m"] = c.night_start_m;
    doc["night_end_h"] = c.night_end_h;
    doc["night_end_m"] = c.night_end_m;
    doc["night_max_power"] = c.night_max_power;

    doc["ota_pass"] = c.ota_pass;
}

// Liest die Werte zurück aus dem JSON-Dokument in das Config-struct.
// `doc["key"] | Standardwert` ist ArduinoJson-Syntax für "lies den Wert, und
// falls der Schlüssel fehlt (z.B. weil eine ältere config.json noch nicht
// alle Felder kennt), nimm stattdessen den Standardwert" -- das verhindert
// abstürzende oder falsche Werte bei fehlenden/neuen Feldern.
// strlcpy() statt eines einfachen "=" kopiert Texte sicher in die festen
// char-Arrays des Config-structs: es schneidet automatisch ab, falls die
// Quelle länger als das Ziel-Array ist, statt (wie strcpy) über das Ende des
// Arrays hinauszuschreiben und Speicher zu beschädigen.
// Übernimmt aus doc NUR die Felder, die dort tatsächlich enthalten sind --
// alles andere in c bleibt unverändert stehen (Merge statt Ersetzen). Das ist
// wichtig, weil das Webinterface jetzt auch Teil-Speicherungen schickt (z.B.
// nur die WLAN-Felder beim Klick auf "WLAN speichern") -- ein Vollersatz mit
// Default-Werten für fehlende Felder würde dabei alle anderen, gerade nicht
// gesendeten Einstellungen (MQTT, Regelung, ...) auf ihre Werkseinstellung
// zurücksetzen. Der `!doc["x"].isNull()`-Check ist deshalb kein Zufall, sondern
// die entscheidende Bedingung -- er ersetzt die früheren `doc["x"] | default`-
// Aufrufe, die bei fehlendem Feld einen Default statt "unverändert lassen"
// eingesetzt hätten.
//
// Für den Boot-Fall (Laden von /config.json) ist das genauso korrekt: main.cpp
// ruft vorher configSetDefaults(config) auf, sodass c beim Aufruf hier schon
// mit sinnvollen Werkseinstellungen gefüllt ist -- fehlt ein Feld in der Datei
// (z.B. weil sie mit einer älteren Firmware-Version gespeichert wurde), bleibt
// einfach der Default aus configSetDefaults() stehen.
static void docToConfig(JsonDocument &doc, Config &c) {
    if (!doc["wifi_ssid"].isNull()) strlcpy(c.wifi_ssid, doc["wifi_ssid"], sizeof(c.wifi_ssid));
    if (!doc["wifi_pass"].isNull()) strlcpy(c.wifi_pass, doc["wifi_pass"], sizeof(c.wifi_pass));
    if (!doc["wifi_ssid2"].isNull()) strlcpy(c.wifi_ssid2, doc["wifi_ssid2"], sizeof(c.wifi_ssid2));
    if (!doc["wifi_pass2"].isNull()) strlcpy(c.wifi_pass2, doc["wifi_pass2"], sizeof(c.wifi_pass2));
    if (!doc["wifi_static"].isNull()) c.wifi_static = doc["wifi_static"];
    if (!doc["wifi_ip"].isNull()) strlcpy(c.wifi_ip, doc["wifi_ip"], sizeof(c.wifi_ip));
    if (!doc["wifi_gw"].isNull()) strlcpy(c.wifi_gw, doc["wifi_gw"], sizeof(c.wifi_gw));
    if (!doc["wifi_mask"].isNull()) strlcpy(c.wifi_mask, doc["wifi_mask"], sizeof(c.wifi_mask));
    if (!doc["wifi_11n"].isNull()) c.wifi_11n = doc["wifi_11n"];

    if (!doc["mqtt_enabled"].isNull()) c.mqtt_enabled = doc["mqtt_enabled"];
    if (!doc["mqtt_broker"].isNull()) strlcpy(c.mqtt_broker, doc["mqtt_broker"], sizeof(c.mqtt_broker));
    if (!doc["mqtt_port"].isNull()) c.mqtt_port = doc["mqtt_port"];
    if (!doc["mqtt_user"].isNull()) strlcpy(c.mqtt_user, doc["mqtt_user"], sizeof(c.mqtt_user));
    if (!doc["mqtt_pass"].isNull()) strlcpy(c.mqtt_pass, doc["mqtt_pass"], sizeof(c.mqtt_pass));
    if (!doc["mqtt_sub_topic"].isNull()) strlcpy(c.mqtt_sub_topic, doc["mqtt_sub_topic"], sizeof(c.mqtt_sub_topic));
    if (!doc["mqtt_input_format"].isNull()) {
        if (doc["mqtt_input_format"].is<uint8_t>() &&
            doc["mqtt_input_format"].as<uint8_t>() <= MQTT_INPUT_SHELLY_JSON) {
            c.mqtt_input_format = doc["mqtt_input_format"].as<uint8_t>();
        } else {
            LOG("Config: ungültiges MQTT-Eingabeformat, Einstellung unverändert");
        }
    }
    if (!doc["mqtt_pub_topic"].isNull()) strlcpy(c.mqtt_pub_topic, doc["mqtt_pub_topic"], sizeof(c.mqtt_pub_topic));

    if (!doc["mode"].isNull()) c.mode = doc["mode"];
    if (!doc["static_watt"].isNull()) c.static_watt = doc["static_watt"];

    if (!doc["shelly_ip"].isNull()) strlcpy(c.shelly_ip, doc["shelly_ip"], sizeof(c.shelly_ip));
    if (!doc["shelly_l1"].isNull()) c.shelly_l1 = doc["shelly_l1"];
    if (!doc["shelly_l2"].isNull()) c.shelly_l2 = doc["shelly_l2"];
    if (!doc["shelly_l3"].isNull()) c.shelly_l3 = doc["shelly_l3"];
    if (!doc["poll_interval_ms"].isNull()) c.poll_interval_ms = doc["poll_interval_ms"];
    if (!doc["shelly_export_ip"].isNull()) strlcpy(c.shelly_export_ip, doc["shelly_export_ip"], sizeof(c.shelly_export_ip));
    if (!doc["shelly_export_l1"].isNull()) c.shelly_export_l1 = doc["shelly_export_l1"];
    if (!doc["shelly_export_l2"].isNull()) c.shelly_export_l2 = doc["shelly_export_l2"];
    if (!doc["shelly_export_l3"].isNull()) c.shelly_export_l3 = doc["shelly_export_l3"];

    if (!doc["json_url"].isNull()) strlcpy(c.json_url, doc["json_url"], sizeof(c.json_url));
    if (!doc["json_path"].isNull()) strlcpy(c.json_path, doc["json_path"], sizeof(c.json_path));

    if (!doc["max_power"].isNull()) c.max_power = doc["max_power"];
    if (!doc["soyo_count"].isNull()) c.soyo_count = doc["soyo_count"];
    if (!doc["calibration_offset_w"].isNull()) c.calibration_offset_w = doc["calibration_offset_w"];
    if (!doc["fallback_watt"].isNull()) c.fallback_watt = doc["fallback_watt"];
    if (!doc["rs485_send_interval_ms"].isNull()) c.rs485_send_interval_ms = doc["rs485_send_interval_ms"];
    if (!doc["tolerance_import_w"].isNull()) c.tolerance_import_w = doc["tolerance_import_w"];
    if (!doc["tolerance_export_w"].isNull()) c.tolerance_export_w = doc["tolerance_export_w"];
    if (!doc["correction_gain_percent"].isNull()) c.correction_gain_percent = doc["correction_gain_percent"];

    if (!doc["night_mode_enabled"].isNull()) c.night_mode_enabled = doc["night_mode_enabled"];
    if (!doc["night_start_h"].isNull()) c.night_start_h = doc["night_start_h"];
    if (!doc["night_start_m"].isNull()) c.night_start_m = doc["night_start_m"];
    if (!doc["night_end_h"].isNull()) c.night_end_h = doc["night_end_h"];
    if (!doc["night_end_m"].isNull()) c.night_end_m = doc["night_end_m"];
    if (!doc["night_max_power"].isNull()) c.night_max_power = doc["night_max_power"];

    if (!doc["ota_pass"].isNull()) strlcpy(c.ota_pass, doc["ota_pass"], sizeof(c.ota_pass));

    // Sicherheitsnetz gegen ungültige/manipulierte Werte aus einer hochgeladenen
    // config.json, z.B. via /config_upload -- soyo_count=0 würde später eine
    // Division durch 0 in der Regellogik (main.cpp) verursachen.
    if (c.soyo_count < 1) c.soyo_count = 1;
    if (c.soyo_count > 12) c.soyo_count = 12;

    // 400ms Untergrenze: darunter blockiert der HTTP-Poll (siehe shelly.cpp)
    // loop() zu stark (live gemessen: ~100-450ms pro Abfrage). 2000ms Obergrenze
    // ist eine willkürliche, aber sinnvolle Obergrenze für die UI.
    if (c.poll_interval_ms < 400) c.poll_interval_ms = 400;
    if (c.poll_interval_ms > 2000) c.poll_interval_ms = 2000;

    // 1000-3000ms: Bei einem BavarianSuperGuy/KlausLi-Referenzcontroller mit
    // identischer Hardware läuft der reale Sende-/Entscheidungszyklus empirisch
    // bestätigt stabil in diesem Bereich (siehe main.cpp-Kommentar) -- 1100ms
    // ist der hier verwendete Default. 1000-3000ms als Testbereich, um das in
    // kontrollierten Testreihen selbst zu verifizieren, ohne in einen Bereich
    // zu geraten, der laut Community-Dokumentation den Wechselrichter zum
    // Stoppen bringen kann.
    if (c.rs485_send_interval_ms < 1000) c.rs485_send_interval_ms = 1000;
    if (c.rs485_send_interval_ms > 3000) c.rs485_send_interval_ms = 3000;

    // 5-50W je Richtung: darunter würde reines Messrauschen (siehe main.cpp)
    // fast jeden Zyklus eine Korrektur auslösen, darüber reagiert die Regelung
    // auf echte kleine Lastwechsel spürbar zu träge. 10W als Default (beide
    // Richtungen gleich) liegt im Bereich der beobachteten Grundrauschen-
    // Streuung typischer Stromzähler -- wer strikte Nulleinspeisung will,
    // kann tolerance_export_w unabhängig enger setzen als tolerance_import_w.
    if (c.tolerance_import_w < 5) c.tolerance_import_w = 5;
    if (c.tolerance_import_w > 50) c.tolerance_import_w = 50;
    if (c.tolerance_export_w < 5) c.tolerance_export_w = 5;
    if (c.tolerance_export_w > 50) c.tolerance_export_w = 50;

    // 10-100%: unter 10% würde die Regelung auf reale Lastwechsel kaum noch
    // reagieren (viel zu träge), 100% entspricht der alten, ungedämpften
    // Vollkorrektur pro Regeldurchlauf.
    if (c.correction_gain_percent < 10) c.correction_gain_percent = 10;
    if (c.correction_gain_percent > 100) c.correction_gain_percent = 100;
}

// Muss als Erstes aufgerufen werden, bevor irgendetwas anderes auf LittleFS
// zugreift. Falls das Dateisystem beschädigt ist (z.B. nach einem Firmware-
// Wechsel mit anderer Partitionsgröße) und sich nicht mounten lässt, wird es
// einmalig formatiert -- danach ist es leer, aber wieder benutzbar.
bool storageBegin() {
    if (!LittleFS.begin()) {
        LOG("LittleFS: Mount fehlgeschlagen, formatiere neu");
        LittleFS.format();
        return LittleFS.begin();
    }
    return true;
}

bool configFileExists() {
    return LittleFS.exists(CONFIG_PATH);
}

bool loadConfig(Config &c) {
    if (!configFileExists()) {
        LOG("Config: /config.json nicht vorhanden");
        return false;
    }

    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) {
        LOG("Config: Öffnen fehlgeschlagen");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        LOG(("Config: JSON-Fehler " + String(err.c_str())).c_str());
        return false;
    }

    docToConfig(doc, c);
    return true;
}

bool saveConfig(const Config &c) {
    JsonDocument doc;
    configToDoc(c, doc);

    File f = LittleFS.open(CONFIG_PATH, "w");
    if (!f) {
        LOG("Config: Schreiben fehlgeschlagen (open)");
        return false;
    }

    bool ok = serializeJson(doc, f) > 0;
    f.close();

    if (!ok) {
        LOG("Config: Serialisierung fehlgeschlagen");
    }
    return ok;
}

String configToJsonString(const Config &c) {
    JsonDocument doc;
    configToDoc(c, doc);
    String out;
    serializeJson(doc, out);
    return out;
}

// Felder, die erst nach einem Neustart wirksam werden, weil die zugehörige
// Bibliothek/Verbindung nur einmal in setup() initialisiert wird (WiFi.begin(),
// mqttClient.setServer(), ElegantOTA.begin()). Alle anderen Felder werden von
// der jeweiligen Loop-Funktion bei jedem Durchlauf frisch aus config gelesen
// und wirken deshalb sofort, ganz ohne Neustart.
static bool jsonNeedsRestart(JsonDocument &doc) {
    static const char *restartFields[] = {
        "wifi_ssid", "wifi_pass", "wifi_ssid2", "wifi_pass2", "wifi_static", "wifi_ip", "wifi_gw", "wifi_mask", "wifi_11n",
        "mqtt_enabled", "mqtt_broker", "mqtt_port", "mqtt_user", "mqtt_pass",
        "mqtt_sub_topic", "mqtt_input_format", "mqtt_pub_topic", "ota_pass"
    };
    for (const char *field : restartFields) {
        if (!doc[field].isNull()) return true;
    }
    return false;
}

bool configMergeFromJsonString(Config &c, const String &json, bool &outNeedsRestart) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        LOG(("Config: Upload JSON-Fehler " + String(err.c_str())).c_str());
        return false;
    }
    outNeedsRestart = jsonNeedsRestart(doc);
    docToConfig(doc, c);
    return true;
}

void factoryReset() {
    LOG("Werksreset: formatiere LittleFS");
    LittleFS.format();
    ESP.restart();
}
