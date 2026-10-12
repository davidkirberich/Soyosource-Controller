#include "rs485.h"

#include "config.h"
#include "telnet_log.h"
#include "safety_logic.h"

int32_t    g_demand = 0;
bool       g_notaus = false;
bool       g_fallbackActive = false;
SoyoStatus g_soyoStatus = {false, SOYO_OP_NORMAL, 0, 0, 0, 0, 0, 0};

static int32_t targetDemand = 0;
static int32_t lastSentDemand = -1; // erzwingt den ersten Sendevorgang
static bool    paused = false;

// Wie oft ein neuer Sollwert gesendet wird -- konfigurierbar über
// config.rs485_send_interval_ms (Webinterface: Regelung), siehe Kommentar
// dort für den Hintergrund (Soyo reagiert bei zu häufigen Updates
// unvorhersehbar).
static unsigned long lastSendMillis = 0;

static unsigned long lastStatusRequestMillis = 0;
static const unsigned long STATUS_REQUEST_INTERVAL_MS = 10000;

// Frühere Version hielt DE/RE zeitgesteuert (fester Timer) auf HIGH und gab
// erst beim nächsten loop()-Durchlauf wieder frei -- das hing davon ab, dass
// rs485Loop() rechtzeitig erneut aufgerufen wird. shellyLoop() blockiert aber
// laut eigenem Kommentar dort 100-450ms pro HTTP-Abfrage: lag ein Sendevorgang
// direkt davor, blieb DE weit über die eigentlich nötigen ~17ms hinaus auf
// HIGH hängen und hat die Gegenseite auf dem gemeinsamen Half-Duplex-Bus
// unnötig lange am Antworten/Senden gehindert. sendFrame() unten wartet jetzt
// stattdessen synchron per Serial.flush(), bis das Frame wirklich komplett
// gesendet ist, und schaltet DE im selben Aufruf sofort zurück -- keine
// Abhängigkeit mehr vom Timing des nächsten loop()-Durchlaufs.

// Antwort-Frame (15 Byte, Header 0x23) ist länger als die Sende-Frames (8 Byte,
// Header 0x24) und beginnt mit einem anderen Header-Byte.
static const uint8_t RX_FRAME_LEN = 15;
static const uint8_t RX_FRAME_HEADER = 0x23;
static uint8_t rxBuffer[RX_FRAME_LEN];
static uint8_t rxIndex = 0;
static unsigned long lastRxByteMillis = 0;

// Bei 4800 Baud kommt ein 15-Byte-Frame als ein zusammenhängender Schwall
// Bytes an (~31ms). Geht dabei ein Byte verloren (z.B. durch eine kurze
// Störung auf dem Bus), würde rxIndex sonst für immer >0 bleiben und ewig auf
// die fehlenden Bytes warten -- jede spätere, an sich saubere Antwort würde
// nie mehr korrekt eingelesen. RX_IDLE_TIMEOUT_MS fängt das ab: kommt mitten
// in einem angefangenen Frame für diese Zeit kein weiteres Byte an, gilt der
// Frame als verloren und wird verworfen, damit auf den nächsten Header-Byte
// (0x23) neu synchronisiert werden kann.
static const unsigned long RX_IDLE_TIMEOUT_MS = 50;

// Sanity-Grenzen für die Status-Response, um Fehl-Synchronisation auf dem
// Bus (verschobene/verstümmelte Frames) zu erkennen und zu verwerfen.
static const float SANITY_MAX_BATTERY_V = 150.0f;
static const float SANITY_MAX_AC_V = 300.0f;
static const float SANITY_MAX_TEMP_C = 200.0f;

// Schreibt denselben Pegel auf beide DE/RE-Pins (siehe Erklärung in rs485.h,
// warum DE und RE immer denselben Wert bekommen). Ist RS485_RE_PIN nicht
// angeschlossen (DE/RE stattdessen auf dem Modul gebrückt), hat dieser
// zusätzliche digitalWrite() einfach keine Wirkung.
static void setDE(bool high) {
    digitalWrite(RS485_DE_RE_PIN, high ? HIGH : LOW);
    digitalWrite(RS485_RE_PIN, high ? HIGH : LOW);
}

static void logFrameHex(const char *prefix, const uint8_t *frame, uint8_t len) {
    String hex;
    for (uint8_t i = 0; i < len; i++) {
        if (frame[i] < 0x10) hex += "0";
        hex += String(frame[i], HEX);
        hex += " ";
    }
    LOG((String(prefix) + hex).c_str());
}

static void sendFrame(const uint8_t *frame, uint8_t len) {
    setDE(true);
    Serial.write(frame, len);
    Serial.flush(); // blockiert bis der UART-Sendepuffer wirklich leer ist (~16,7ms bei 8 Byte/4800 Baud)
    setDE(false);
    logFrameHex("RS485 TX: ", frame, len);
}

// Baut das 8-Byte-Kommando, mit dem der Soyosource-Wechselrichter angewiesen
// wird, genau "watt" Watt einzuspeisen. Byte 0-3 sind ein fester, vom
// Wechselrichter vorgegebener Befehls-Header (siehe README). Byte 4/5 sind
// der Watt-Wert, aufgeteilt in High-Byte und Low-Byte (siehe README, Abschnitt
// "Für Einsteiger" zu <<8 und |). Byte 7 ist eine simple Prüfsumme, mit der
// der Wechselrichter erkennt, ob die Übertragung fehlerfrei war; die Formel
// "264 - byte[4] - byte[5]" ist vom Hersteller/Protokoll fest vorgegeben,
// keine eigene Erfindung.
static void sendDemandFrame(uint16_t watt) {
    uint8_t frame[8];
    buildDemandFrame(watt, frame);
    sendFrame(frame, 8);
}

static void sendStatusRequestFrame() {
    static const uint8_t frame[8] = {0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    sendFrame(frame, 8);
}

// Status-Response-Layout (15 Byte):
//   [0]     0x23 Header
//   [1]     0x01
//   [2]     0x01
//   [3]     0x00 Reserved
//   [4]     Operation-Status (siehe SoyoOperationStatus)
//   [5..6]  Batteriespannung  uint16 BE, x0.1V
//   [7..8]  Batteriestrom     uint16 BE, x0.1A
//   [9..10] AC-Spannung       uint16 BE, x1V
//   [11]    AC-Frequenz       uint8,     x0.5Hz
//   [12..13] Temperatur       uint16 BE, (raw-300) x0.1°C
//   [14]    CRC (Algorithmus nicht bekannt, wird nicht geprüft)
//
// Hinweis: Neuere Geräte (purple mainboard, FW STC8-2022-218) antworten
// grundsätzlich nicht auf den Status-Request. Das ist kein Fehler -- es
// kommt einfach keine Response an, g_soyoStatus.valid bleibt false und die
// Soyo-Statuszeile im Webinterface bleibt leer/ausgeblendet.
static void parseStatusResponse(const uint8_t *frame) {
    // Nicht nur Byte 0 (Header) pruefen, sondern wie syssi/esphome-soyosource-
    // gtn-virtual-meter auch Byte 1-3 gegen das erwartete, feste Muster
    // 0x01 0x01 0x00 -- ein Frame, das nur zufaellig mit 0x23 beginnt, aber
    // sonst verschoben/verstuemmelt ist, faellt so schon hier auf, statt sich
    // erst auf die deutlich groberen Plausibilitaetsgrenzen weiter unten
    // (Batteriespannung/AC-Spannung/Temperatur) verlassen zu muessen.
    if (frame[0] != RX_FRAME_HEADER || frame[1] != 0x01 || frame[2] != 0x01 || frame[3] != 0x00) {
        LOG("RS485: Status-Response verworfen (falscher Header)");
        return;
    }

    // Jeweils zwei Bytes zu einer 16-Bit-Zahl zusammensetzen (siehe README,
    // Abschnitt "Für Einsteiger" zu <<8 und |).
    uint16_t battRaw = (frame[5] << 8) | frame[6];
    uint16_t currRaw = (frame[7] << 8) | frame[8];
    uint16_t acRaw = (frame[9] << 8) | frame[10];
    uint16_t tempRaw = (frame[12] << 8) | frame[13];

    float batteryVoltage = battRaw * 0.1f;
    float acVoltage = acRaw * 1.0f;
    float temperature = ((int32_t)tempRaw - 300) * 0.1f;

    if (batteryVoltage >= SANITY_MAX_BATTERY_V ||
        acVoltage >= SANITY_MAX_AC_V ||
        temperature >= SANITY_MAX_TEMP_C) {
        LOG("RS485: Status-Response verworfen (Sanity-Check fehlgeschlagen)");
        return;
    }

    g_soyoStatus.operationStatus = frame[4];
    g_soyoStatus.batteryVoltage = batteryVoltage;
    g_soyoStatus.batteryCurrent = currRaw * 0.1f;
    g_soyoStatus.acVoltage = acVoltage;
    g_soyoStatus.frequency = frame[11] * 0.5f;
    g_soyoStatus.temperature = temperature;
    g_soyoStatus.valid = true;
    g_soyoStatus.lastUpdate = millis();

    LOG(("RS485: Status-Response empfangen (Status=" + String(g_soyoStatus.operationStatus) + ")").c_str());
}

// Sendet den Sollwert bei JEDEM Aufruf neu, unabhängig davon, ob er sich
// seit dem letzten Mal geändert hat -- keine "nur bei Änderung senden"-
// Optimierung mehr. Grund: genau das Ausbleiben von RS485-Frames bei länger
// unverändertem Sollwert hat nachweislich (siehe main.cpp-Kommentar zur
// Toleranzband-Formel sowie ein 2 Jahre altes, selbst erlebtes Bug-Ticket zu
// BavarianSuperGuys Original-Firmware) dazu geführt, dass der Soyo nach einer
// Weile Funkstille die Einspeisung von sich aus abschaltet -- worauf der
// nächste, dann meist große Regeleingriff zu genau dem Sägezahn-Schwingen
// führte, das wir hier über weite Teile des Tages gejagt haben. Passt auch
// zu BavarianSuperGuys späterem, per README dokumentiertem Fix ("Der esp
// ansich schickt schon jede Sekunde den zuletzt angenommenen Wert").
static void applyDemand() {
    if (g_notaus) {
        targetDemand = 0;
    }
    g_demand = targetDemand;

    sendDemandFrame((uint16_t)g_demand);
    LOG(("RS485: Demand " + String(lastSentDemand) + "W -> " + String(g_demand) + "W").c_str());
    lastSentDemand = g_demand;
}

void rs485EmergencyStop() {
    targetDemand = 0;
    if (paused) return;

    g_demand = 0;
    sendDemandFrame(0);
    lastSentDemand = 0;
    lastSendMillis = millis();
    LOG("RS485: Notaus-Frame 0W sofort gesendet");
}

void rs485Begin() {
    pinMode(RS485_DE_RE_PIN, OUTPUT);
    pinMode(RS485_RE_PIN, OUTPUT);
    setDE(false); // erstmal auf Empfang schalten
    // 4800 Baud, 8 Datenbits, kein Paritätsbit, 1 Stoppbit -- feste Vorgabe
    // des Soyosource-Protokolls, nicht verhandelbar.
    Serial.begin(4800, SERIAL_8N1);
}

// Wird bei jedem Schleifendurchlauf aus der Haupt-loop() (main.cpp) aufgerufen.
// Macht bei jedem Aufruf nur wenig Arbeit und kehrt schnell zurück -- so bleibt
// die Firmware insgesamt reaktionsschnell (kein delay(), kein Blockieren).
void rs485Loop() {
    // Angefangenes, aber nicht rechtzeitig vervollständigtes Frame verwerfen
    // (siehe Kommentar bei RX_IDLE_TIMEOUT_MS oben) und neu synchronisieren.
    if (rxIndex > 0 && millis() - lastRxByteMillis >= RX_IDLE_TIMEOUT_MS) {
        LOG("RS485: unvollständige Status-Response verworfen (Timeout)");
        rxIndex = 0;
    }

    // Empfangene Bytes einsammeln, bis ein komplettes 15-Byte-Antwort-Frame
    // beisammen ist. Serial.available()/read() liefern immer nur einzelne,
    // bereits angekommene Bytes -- ein ganzes Frame kommt über mehrere
    // Schleifendurchläufe verteilt an.
    while (Serial.available()) {
        uint8_t b = Serial.read();
        lastRxByteMillis = millis();
        if (rxIndex == 0 && b != RX_FRAME_HEADER) {
            continue; // auf Frame-Start (0x23) warten
        }
        rxBuffer[rxIndex++] = b;
        if (rxIndex >= RX_FRAME_LEN) {
            parseStatusResponse(rxBuffer);
            rxIndex = 0;
        }
    }

    if (paused) {
        return; // OTA-Update läuft, RS485 pausiert (siehe rs485Pause)
    }

    unsigned long now = millis();

    // Sollwert-Senden hat klar Vorrang vor dem Status-Request, da der Bus
    // Half-Duplex ist (siehe rs485.h) und pro Durchlauf nur ein Frame
    // rausgeht: zuverlässiges, regelmäßiges Senden ist entscheidend dafür,
    // dass der Soyo nicht wegen Funkstille die Einspeisung abschaltet (siehe
    // Kommentar bei applyDemand) -- der Status-Request liefert dagegen nur
    // Diagnosewerte fürs Webinterface und darf ohne Weiteres eine Runde
    // aussetzen, wenn er mit einem fälligen Sendezyklus kollidiert.
    if (now - lastSendMillis >= config.rs485_send_interval_ms) {
        lastSendMillis = now;
        applyDemand();
    } else if (now - lastStatusRequestMillis >= STATUS_REQUEST_INTERVAL_MS) {
        lastStatusRequestMillis = now;
        sendStatusRequestFrame();
    }
}

void rs485SetTargetDemand(int32_t watts) {
    if (watts < 0) watts = 0;
    targetDemand = watts;
}

void rs485Pause(bool pause) {
    paused = pause;
    if (pause) {
        setDE(false);
        LOG("RS485: pausiert (OTA)");
    } else {
        LOG("RS485: fortgesetzt");
    }
}
