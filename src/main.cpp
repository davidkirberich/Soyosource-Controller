// Dieses File verbindet nur die einzelnen Module miteinander -- es enthält
// selbst möglichst wenig Fachlogik. Jedes Modul (wifi_mgr, led, rs485, shelly,
// mqtt_mgr, http_server, ota) bringt sein eigenes <modul>Begin() (einmalige
// Einrichtung) und <modul>Loop() (wird bei jedem Schleifendurchlauf
// aufgerufen) mit. setup() ruft alle Begin()-Funktionen einmal auf, loop()
// ruft alle Loop()-Funktionen wieder und wieder auf. setup() und loop() sind
// dabei keine frei erfundenen Namen: Arduino ruft automatisch setup() genau
// einmal nach dem Booten auf und danach loop() endlos immer wieder, solange
// der ESP8266 läuft -- man muss (und darf) sie nirgendwo selbst aufrufen.
#include <Arduino.h>
#include <ESP8266WiFi.h>

#include "config.h"
#include "storage.h"
#include "telnet_log.h"
#include "wifi_mgr.h"
#include "led.h"
#include "ntp.h"
#include "rs485.h"
#include "shelly.h"
#include "mqtt_mgr.h"
#include "http_server.h"
#include "ota.h"
#include "display.h"
#include "safety_logic.h"

// Die einzige Config-Instanz im ganzen Projekt (siehe "extern Config config;"
// in config.h -- dort wird sie nur bekanntgegeben, hier tatsächlich angelegt).
Config config;

LoopTimingStats g_loopTiming;
static uint64_t loopDurationSumUs = 0;
static uint32_t loopSamples = 0;
static uint32_t loopMaximumUs = 0;
static unsigned long loopWindowStartMillis = 0;

static unsigned long lastHeapLogMillis = 0;
static const unsigned long HEAP_LOG_INTERVAL_MS = 30000;
static const uint32_t HEAP_MIN_BYTES = 4096;
static const unsigned long MEASUREMENT_STALE_MS = 60000;

// GPIO0 (Flash-Button) 5s beim Boot gedrückt halten -> Werksreset.
//
// Das ist die einzige Stelle im ganzen Projekt, die eine blockierende
// while()-Schleife verwendet -- überall sonst gilt strikt "kein delay(), kein
// blockierendes while()". Das ist hier bewusst so, denn erstens läuft diese
// Funktion ganz am Anfang von setup(), also BEVOR WLAN, Webserver usw.
// überhaupt gestartet sind (es gibt also noch nichts, was durch das Warten
// "blockiert" werden könnte), und zweitens SOLL das Gerät hier ja genau
// 5 Sekunden lang auf das Loslassen der Taste warten. yield() innerhalb der
// Schleife ist trotzdem wichtig: es gibt dem ESP8266 kurz Zeit für interne
// Haushaltsaufgaben (z.B. WLAN-Chip bedienen); ohne yield() würde der interne
// Hardware-Watchdog (siehe runWatchdog() unten) das Gerät nach ein paar
// Sekunden Stillstand fälschlich neu starten.
static void checkFactoryReset() {
    pinMode(0, INPUT_PULLUP);
    if (digitalRead(0) != LOW) {
        return;
    }

    unsigned long start = millis();
    while (millis() - start < 5000) {
        if (digitalRead(0) != LOW) {
            return; // Taste vorzeitig losgelassen
        }
        yield();
    }

    if (digitalRead(0) == LOW) {
        factoryReset(); // formatiert LittleFS und startet neu, kehrt nicht zurück
    }
}

// Regellogik: messwert (inkl. Offset, in g_netzwert) -> neuer Sollwert.
// Static-Modus hält konstant static_watt, Fallback-Zustand fallback_watt.
// Toleranzband und Nachtmodus-Begrenzung gelten nur für die reguläre
// messwertbasierte Regelung.
//
// Die Kernformel "target = g_demand + netz / soyo_count" ist eine einfache
// Nachführung, kein exakter Sprung auf den "richtigen" Wert: g_netzwert ist
// die aktuell am Stromzähler gemessene Differenz zum Nullpunkt (positiv =
// Haus bezieht noch Strom vom Netz, der Soyo müsste also MEHR einspeisen).
// Beispiel: aktueller Sollwert 400W, Messwert +60W, soyo_count=1 -> neuer
// Sollwert 400 + 60 = 460W. Bei jedem folgenden Regeldurchlauf wird so erneut
// nachgesteuert, bis sich Verbrauch und Einspeisung ungefähr die Waage halten
// (dann pendelt g_netzwert nahe 0 und landet im Toleranzband, siehe unten).
//
// Diese Formel (inkl. Toleranzband) wurde anhand von Live-Messdaten eines
// tatsächlich laufenden BavarianSuperGuy/KlausLi-Controllers mit identischer
// Hardware verifiziert: über 17 beobachtete Sollwert-Sprünge stimmt
// "delta = aktueller_Rohmesswert / soyo_count" mit
// durchschnittlich <0.5W Abweichung -- ein direkter Rohwert-Bezug, kein
// gleitender Durchschnitt (der getestet und anhand derselben Daten verworfen
// wurde: je größer das Mittelungsfenster, desto schlechter die Vorhersage).
//
// runControlLoop() selbst läuft bei jedem loop()-Durchlauf (also sehr oft,
// nicht nur alle paar hundert ms), ein neuer Messwert kommt aber nur an, wenn
// shellyLoop()/MQTT/HttpInterface tatsächlich etwas Neues liefern -- diese
// beiden Takte sind bewusst entkoppelt (siehe shelly.cpp). Ohne die Prüfung
// unten würde derselbe, bereits verarbeitete Messwert bei jedem Durchlauf
// erneut auf g_demand aufaddiert werden, obwohl sich in der Realität nichts
// geändert hat -- der Sollwert würde sich künstlich aufschaukeln, statt nur
// einmal pro echter Messung nachgeführt zu werden.
static unsigned long lastProcessedMeasurementMillis = 0;
static int32_t lastComputedTarget = 0;

static void runControlLoop() {
    if (g_notaus) {
        rs485SetTargetDemand(0);
        return;
    }

    int32_t target;

    // static_watt/fallback_watt sind als GESAMTLEISTUNG des ganzen Systems
    // gedacht (z.B. "Fallback auf 200W" meint 200W insgesamt, nicht pro
    // Inverter) -- rs485SetTargetDemand() schickt denselben Sollwert aber
    // unveraendert an JEDEN der soyo_count parallelen Inverter (jeder fuehrt
    // ihn unabhaengig aus), die tatsaechliche Gesamtleistung ist also
    // target * soyo_count. Deshalb hier durch soyo_count teilen, bevor der
    // Wert als Sollwert PRO Inverter gesendet wird.
    //
    // max_power/night_max_power sind dagegen bewusst als PRO-INVERTER-Wert
    // gedacht (z.B. "2000" = Typenschild-Maximalwert eines einzelnen
    // Wechselrichters) und werden deshalb weiter unten OHNE Division direkt
    // als Pro-Geraet-Obergrenze verwendet -- ein Nutzer mit ungleich starken
    // Invertern (z.B. 3x 2000W + 1x 200W am selben Bus, der schwaechere
    // deckelt sich ohnehin selbst) kann so einfach den echten Maximalwert
    // seiner staerksten Geraete eintragen, statt selbst Summe/soyo_count
    // ausrechnen zu muessen.
    if (config.mode == MODE_STATIC) {
        target = distributeTotalDemand(config.static_watt, config.soyo_count);
        g_lastMeasurementMillis = millis(); // kein externer Messwert im Static-Modus nötig
    } else if (g_fallbackActive) {
        target = distributeTotalDemand(config.fallback_watt, config.soyo_count);
    } else if (g_lastMeasurementMillis != lastProcessedMeasurementMillis) {
        // Es liegt ein Messwert vor, der seit dem letzten Regeldurchlauf noch
        // nicht verarbeitet wurde -- jetzt (und nur jetzt) einmalig nachführen.
        lastProcessedMeasurementMillis = g_lastMeasurementMillis;

        // Zwei unabhängige Toleranzgrenzen (config.tolerance_import_w/
        // tolerance_export_w, einstellbar im Webinterface unter "Regelung"):
        // korrigiert erst, wenn der Messwert in die jeweilige Richtung die
        // dafür geltende Grenze erreicht/überschreitet. Standardmäßig beide
        // gleich (symmetrisch), aber unabhängig verstellbar -- wer z.B.
        // striktere Nulleinspeisung will, setzt tolerance_export_w enger als
        // tolerance_import_w.
        float netz = g_netzwert;
        if (netz >= (float)config.tolerance_import_w || netz <= -(float)config.tolerance_export_w) {
            // Gedämpft statt voll: siehe correction_gain_percent in config.h --
            // verhindert ein Aufschaukeln, wenn die tatsächliche Wirkung einer
            // Korrektur dem Messwert um ein paar Regeldurchläufe hinterherhinkt.
            float korrektur = (netz / config.soyo_count) * (config.correction_gain_percent / 100.0f);
            target = g_demand + (int32_t)korrektur;
        } else {
            target = g_demand; // Toleranzband: Sollwert unverändert
        }
        lastComputedTarget = target;
    } else {
        // Kein neuer Messwert seit dem letzten Durchlauf -> zuletzt berechneten
        // Sollwert halten, nicht erneut aus demselben Messwert nachrechnen.
        target = lastComputedTarget;
    }

    target = applyDemandLimits(target, config.max_power, isNightMode(),
                               config.night_max_power);

    rs485SetTargetDemand(target);
}

// "Watchdog" ist ein klassisches Embedded-Konzept: eine Art Wachhund, der die
// Firmware neu startet, falls sie sich mal aufhängen sollte (z.B. durch einen
// Bug, der eine Endlosschleife ohne yield() verursacht). Der ESP8266 hat dafür
// zwei eingebaute Mechanismen: einen Hardware-Watchdog (läuft immer automatisch
// im Hintergrund, reagiert auf fehlendes yield()) und einen Software-Watchdog,
// den man per wdtFeed() regelmäßig "füttern" muss (siehe setup(): wdtEnable),
// sonst startet auch er das Gerät neu. Diese Funktion hier "füttert" ihn bei
// jedem Schleifendurchlauf und ergänzt zusätzlich zwei eigene, projektspezifische
// Sicherheitschecks (Heap-Stand, Messwert-Alter) um dieselbe Idee: lieber
// selbst kontrolliert neu starten, als in einem unklaren Zustand steckenzubleiben.
static void runWatchdog() {
    ESP.wdtFeed();

    unsigned long now = millis();

    if (now - lastHeapLogMillis >= HEAP_LOG_INTERVAL_MS) {
        lastHeapLogMillis = now;
        LOG(("Heap frei: " + String(ESP.getFreeHeap()) + " Bytes").c_str());
    }

    if (ESP.getFreeHeap() < HEAP_MIN_BYTES) {
        LOG("Watchdog: Heap kritisch niedrig, Neustart");
        ESP.restart();
    }

    // Static-Modus liefert nie einen externen Messwert, AP-Modus wartet auf
    // Erstkonfiguration -- beides wäre sonst ein Reboot-Loop. Genauso im
    // Fallback-Zustand (siehe shelly.cpp): g_lastMeasurementMillis wird dort
    // bewusst NICHT aktualisiert, solange die Messwertquelle weiter ausfällt --
    // ohne diese Ausnahme würde der ESP alle 60s neu starten, obwohl der
    // Fallback-Sollwert (Standard 0W) längst sicher und korrekt anliegt. Jeder
    // einzelne erfolgreiche Poll aktualisiert g_lastMeasurementMillis wieder
    // (siehe applyMeasurement()), auch während man noch im Fallback steckt --
    // die Ausnahme hier wird also nicht dauerhaft "scharf" gestellt, sobald
    // die Quelle zwischenzeitlich mal wieder antwortet.
    bool measurementRelevant = (config.mode != MODE_STATIC) && !wifiMgrIsApMode() && !g_fallbackActive;
    if (measurementRelevant && now - g_lastMeasurementMillis > MEASUREMENT_STALE_MS) {
        LOG("Watchdog: Kein gültiger Messwert seit >60s, Neustart");
        ESP.restart();
    }
}

// Läuft genau einmal, direkt nach dem Booten. Reihenfolge ist wichtig:
// zuerst der Werksreset-Check (siehe checkFactoryReset), dann die
// Konfiguration laden (storageBegin/configSetDefaults/loadConfig), erst
// danach das Telnet-Logging starten (config.* wird von manchen Begin()-
// Funktionen schon gebraucht) und zum Schluss OTA, das den bereits von
// httpServerBegin() erzeugten Webserver braucht.
void setup() {
    checkFactoryReset();

    storageBegin();
    configSetDefaults(config); // erst Werkseinstellungen als Basis...
    loadConfig(config);        // ...dann von /config.json überschreiben, falls vorhanden

    telnetLogBegin();
    LOG("Boot: Soyosource-Controller startet");

    ledBegin();
    rs485Begin();
    wifiMgrBegin();
    ntpBegin();
    shellyBegin();
    mqttBegin();
    httpServerBegin();
    otaBegin(server);
    displayBegin();

    g_lastMeasurementMillis = millis();

    ESP.wdtEnable(0); // Software-Watchdog aktivieren (siehe runWatchdog)
    loopWindowStartMillis = millis();
}

// Läuft ab jetzt endlos, wieder und wieder, viele hundert Mal pro Sekunde.
// Jeder Aufruf einer <modul>Loop()-Funktion hier soll schnell zurückkehren
// (kein delay(), kein blockierendes Warten) -- nur so bleiben alle Module
// gleichzeitig "responsive": während z.B. gerade auf eine HTTP-Antwort vom
// Shelly gewartet wird, kann trotzdem weiterhin die Webseite ausgeliefert,
// RS485 bedient und die LED aktualisiert werden.
void loop() {
    const uint32_t loopStartUs = micros();
    wifiMgrLoop();
    ledLoop();
    telnetLogLoop();
    rs485Loop();
    shellyLoop();
    mqttLoop();
    httpServerLoop();
    otaLoop();
    displayLoop();

    runControlLoop();
    runWatchdog();

    // Wall time includes blocking waits, but not background work between loops.
    const uint32_t durationUs = micros() - loopStartUs;
    loopDurationSumUs += durationUs;
    loopSamples++;
    if (durationUs > loopMaximumUs) loopMaximumUs = durationUs;
    if (durationUs > g_loopTiming.peakUs) g_loopTiming.peakUs = durationUs;
    const unsigned long now = millis();
    if (now - loopWindowStartMillis >= 5000) {
        g_loopTiming.averageUs = loopDurationSumUs / loopSamples;
        g_loopTiming.maximumUs = loopMaximumUs;
        g_loopTiming.ready = true;
        loopDurationSumUs = 0;
        loopSamples = 0;
        loopMaximumUs = 0;
        loopWindowStartMillis = now;
    }
}
