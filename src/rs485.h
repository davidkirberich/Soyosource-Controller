#pragma once

#include <Arduino.h>

// RS485 ist eine Zweidraht-Verbindung, auf der (anders als bei USB oder dem
// normalen Serial-Debug-Port) immer nur eine Seite gleichzeitig senden darf
// ("Half-Duplex"). Das RS485-Modul hat dafür zwei Steuerpins: DE ("Driver
// Enable", active HIGH) schaltet den Sendetreiber frei, RE ("Receiver
// Enable", active LOW) schaltet den Empfänger frei. Da man nie beide
// gleichzeitig will (immer entweder senden ODER empfangen), bekommen DE und
// RE in beiden Zuständen zufällig denselben Pegel: HIGH+HIGH beim Senden
// (Empfänger dabei bewusst aus), LOW+LOW beim Empfangen (Sender dabei aus).
//
// Deshalb reicht ein einziger GPIO, wenn man DE und RE auf dem Modul selbst
// zusammenlötet/-brückt (RS485_DE_RE_PIN). Wer das nicht am Modul verlöten
// möchte, kann stattdessen RS485_RE_PIN separat mit dem RE-Pin verbinden --
// die Firmware schreibt auf beide GPIOs ohnehin immer denselben Wert
// (setDE() in rs485.cpp), eine Verbindungslogik ändert sich dadurch nicht.
// Bleibt RS485_RE_PIN unbenutzt (DE/RE weiterhin auf dem Modul gebrückt),
// schadet der zusätzliche digitalWrite() nicht -- der Pin hat dann einfach
// keine Wirkung.
#define RS485_DE_RE_PIN 14 // D5
#define RS485_RE_PIN     12 // D6 (benachbart zu D5), nur nötig wenn DE/RE NICHT auf dem Modul gebrückt sind

// Operation-Status, Byte 4 der Status-Response.
enum SoyoOperationStatus : uint8_t {
    SOYO_OP_NORMAL           = 0x00,
    SOYO_OP_STARTUP          = 0x01,
    SOYO_OP_STANDBY          = 0x02,
    SOYO_OP_STARTUP_ABORTED  = 0x03,
    SOYO_OP_ERROR_BATTERY    = 0x04
};

struct SoyoStatus {
    bool valid;
    uint8_t operationStatus;
    float batteryVoltage;
    float batteryCurrent;
    float acVoltage;
    float frequency;
    float temperature;
    unsigned long lastUpdate;
};

extern int32_t   g_demand;        // aktuell tatsächlich gesendeter Sollwert in Watt
extern bool      g_notaus;        // Notaus aktiv -> Demand sofort 0
extern bool      g_fallbackActive;
extern SoyoStatus g_soyoStatus;

void rs485Begin();
void rs485Loop();

// Setzt den gewünschten Zielsollwert. Begrenzung (max_power, Nachtmodus, ...)
// muss der Aufrufer bereits vorgenommen haben. Sendelogik (nur bei Änderung
// >1W, Intervall siehe config.rs485_send_interval_ms) übernimmt rs485Loop().
void rs485SetTargetDemand(int32_t watts);

// Sendet beim aktivierten Notaus sofort einen 0-W-Sollwert, sofern RS485
// nicht pausiert ist (z.B. während eines OTA-Uploads).
void rs485EmergencyStop();

// Für OTA: stoppt jegliches Senden und hält DE/RE auf Empfang (LOW).
void rs485Pause(bool pause);
