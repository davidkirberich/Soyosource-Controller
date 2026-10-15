#pragma once

#include <Arduino.h>

// Betriebsmodi der Regelung
enum OperatingMode : uint8_t {
    MODE_STATIC          = 0, // konstanter Sollwert = static_watt
    MODE_HTTP_INTERFACE  = 1, // Messwert per GET /L1L2L3Auto?Value=x
    MODE_MQTT_SUB        = 2, // Messwert per MQTT-Subscribe
    MODE_SHELLY_GEN1     = 3, // Shelly Gen1 /status total_power
    MODE_SHELLY_GEN2     = 4, // Shelly Gen2 Pro /rpc/EM.GetStatus
    MODE_JSON_HTTP       = 5  // generischer JSON-HTTP-Client
};

enum MqttInputFormat : uint8_t {
    MQTT_INPUT_NUMBER = 0,
    MQTT_INPUT_SHELLY_JSON = 1
};

// Alle Einstellungen des Geräts in einem einzigen Behälter (struct). Es gibt
// genau ein Config-Objekt im ganzen Projekt (siehe "extern Config config;"
// unten und dessen Definition in main.cpp) — jedes Modul, das eine Einstellung
// braucht, greift auf dasselbe Objekt zu.
//
// Texte werden bewusst als feste char-Arrays (z.B. char wifi_ssid[64])
// gespeichert statt als Arduino-String: die Größe steht damit von vornherein
// fest, was zum 1:1 passenden JSON-Format in /config.json passt und
// Speicherfragmentierung auf dem ESP8266 vermeidet (String wächst/schrumpft
// dynamisch auf dem Heap, was auf Dauer instabil werden kann).
struct Config {
    // WiFi
    char wifi_ssid[64];
    char wifi_pass[64];
    // Fallback-WLAN (optional, leer lassen = kein Fallback): wird erst probiert,
    // wenn das primäre WLAN oben eine Weile nicht erreichbar ist -- z.B. ein
    // Handy-Hotspot oder ein zweiter Router als Notlösung, falls der Haupt-
    // Router mal ausfällt. Das primäre WLAN bleibt dabei immer bevorzugt: läuft
    // das Gerät gerade auf dem Fallback, wird im Hintergrund regelmäßig
    // geprüft, ob das primäre Netz wieder erreichbar ist, und automatisch
    // dorthin zurückgewechselt.
    char wifi_ssid2[64];
    char wifi_pass2[64];
    bool wifi_static;      // true = feste IP unten verwenden statt DHCP
    char wifi_ip[16];
    char wifi_gw[16];
    char wifi_mask[16];
    bool wifi_11n;          // erzwingt WLAN-Standard 802.11n (hilft bei manchen Routern/Störungen)

    // MQTT
    bool     mqtt_enabled;
    char     mqtt_broker[64];
    uint16_t mqtt_port;
    char     mqtt_user[32];
    char     mqtt_pass[32];
    char     mqtt_sub_topic[64];
    uint8_t  mqtt_input_format;
    char     mqtt_pub_topic[64];

    // Betriebsmodus
    uint8_t mode;
    uint16_t static_watt; // GESAMT-Sollwert im Static-Modus (mode=0), siehe
                          // max_power weiter unten -- gleiches Prinzip

    // Shelly / JSON HTTP Client
    char shelly_ip[32];
    bool shelly_l1, shelly_l2, shelly_l3; // nur Gen2 wirksam
    uint16_t poll_interval_ms; // Abfrageintervall Shelly/JSON-HTTP, 400-2000ms

    // Optionaler Shelly-Gen2-Pro-Einspeisezähler
    char shelly_export_ip[32];
    bool shelly_export_l1, shelly_export_l2, shelly_export_l3;

    // JSON HTTP Client
    char json_url[128];
    char json_path[64]; // Punkt-separiert, z.B. "StatusSNS.SML.DJ_TPWRCURR"

    // Regelung
    uint16_t max_power;      // Obergrenze PRO Inverter, in Watt (Sicherheitslimit,
                             // z.B. Typenschild-Maximalwert eines einzelnen
                             // Soyos) -- main.cpp verwendet den Wert direkt,
                             // ohne Division durch soyo_count. Bei ungleich
                             // starken Invertern am selben Bus einfach den
                             // Wert der stärksten Geräte eintragen.
    uint8_t  soyo_count;     // Anzahl parallel angeschlossener Soyo-Geräte (1-12);
                             // der Messwert wird durch diese Zahl geteilt, bevor er
                             // auf den Sollwert aufaddiert wird (siehe main.cpp)
    int16_t  calibration_offset_w; // wird zu jedem Rohmesswert addiert, BEVOR die
                             // Toleranzbänder/Regelung ihn sehen -- gleicht einen
                             // systematisch falsch kalibrierten Stromzähler aus
                             // (z.B. zeigt er immer 5W zu viel an). Kein
                             // Richtungs-Bias wie tolerance_import_w/export_w
                             // (die verändern nur die Totzonen-Breite, nicht den
                             // tatsächlichen Referenzpunkt) -- unabhängige,
                             // orthogonale Einstellung.
    uint16_t fallback_watt;  // GESAMT-Sollwert, wenn die Messwertquelle wiederholt
                             // ausfällt (siehe max_power oben -- gleiches Prinzip,
                             // main.cpp teilt durch soyo_count)
    uint16_t rs485_send_interval_ms; // Wie oft ein neuer Sollwert an den Soyo gesendet
                             // wird, 1000-3000ms, Standard 1100ms. Ein Referenzcontroller
                             // (BavarianSuperGuy/KlausLi) mit identischer Hardware läuft
                             // empirisch bestätigt stabil in diesem Bereich -- eignet sich
                             // für eigene Testreihen.
    uint16_t tolerance_import_w; // Toleranz Richtung Bezug (5-50W, Standard 10W):
                             // solange g_netzwert = tolerance_import_w bleibt, wird
                             // der Sollwert nicht nachgeführt. Unabhängig von
                             // tolerance_export_w einstellbar -- wer z.B. möglichst
                             // strikte Nulleinspeisung will, setzt export eng und
                             // import weiter; wer wie bei uns beide Richtungen
                             // gleich behandeln will, setzt beide gleich.
    uint16_t tolerance_export_w; // Toleranz Richtung Einspeisung (5-50W, Standard
                             // 10W), spiegelbildlich zu tolerance_import_w -- siehe
                             // main.cpp für die genaue Verwendung beider Werte.
    uint8_t  correction_gain_percent; // Dämpfung der Regelkorrektur (10-100%,
                             // Standard 50%): main.cpp wendet pro Regeldurchlauf
                             // nur diesen Prozentsatz von "netz / soyo_count" an,
                             // statt die volle Korrektur auf einmal. Grund: zwischen
                             // "neuer Sollwert gesendet" und "Shelly sieht die
                             // tatsächliche Wirkung am Zähler" liegt eine reale
                             // Verzögerung (Rampzeit des Soyo, Mittelungsfenster des
                             // Shelly) -- reagiert die Regelschleife bei jedem
                             // Messwert sofort mit voller Stärke, kann das zu einem
                             // Aufschaukeln (Sägezahn/Limit-Cycle) führen, weil schon
                             // die nächste Messung noch die alte, ausstehende
                             // Korrektur "sieht" und erneut voll nachlegt. Eine
                             // Dämpfung < 100% lässt jeden einzelnen Schritt kleiner
                             // ausfallen, wodurch sich Verzögerungseffekte über
                             // mehrere Regeldurchläufe hinweg ausmitteln statt
                             // aufzuschaukeln -- kostet etwas Reaktionsgeschwindigkeit,
                             // gewinnt dafür Stabilität.

    // Nachtmodus: begrenzt die maximale Leistung in einem Zeitfenster, z.B. um
    // nachts leiser/schonender zu fahren
    bool     night_mode_enabled;
    uint8_t  night_start_h, night_start_m; // Beginn, z.B. 22:00
    uint8_t  night_end_h,   night_end_m;   // Ende, z.B. 06:00 (darf über Mitternacht gehen)
    uint16_t night_max_power; // PRO-Inverter-Obergrenze wie max_power oben,
                              // nur während des Nachtfensters

    // OTA
    char ota_pass[32];
};

extern Config config;

// Füllt c mit den Werkseinstellungen.
void configSetDefaults(Config &c);
