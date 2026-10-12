#pragma once

#include <Arduino.h>

extern float         g_netzwert;              // letzter Messwert in Watt, inkl. calibration_offset_w
extern unsigned long g_lastMeasurementMillis;
extern float         g_shellyPhasePower[3];
extern bool          g_shellyPhaseValid[3];
extern float         g_shellyExportPhasePower[3];
extern bool          g_shellyExportPhaseValid[3];
extern float         g_shellyExportTotalW;
extern bool          g_shellyExportValid;

void shellyBegin();

// Pollt die konfigurierte Verbrauchsmessquelle und optional den
// Einspeisezähler. Nur Fehler der Verbrauchsmessquelle steuern den Fallback.
void shellyLoop();

// Von HttpInterface- (/L1L2L3Auto) und MQTT-Modus aufgerufen, um einen extern
// gelieferten Rohmesswert (ohne Offset) einzuspeisen.
void applyMeasurement(float rawWatt);
