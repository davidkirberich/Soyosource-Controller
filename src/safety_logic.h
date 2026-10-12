#pragma once

#include <stdint.h>

int32_t distributeTotalDemand(uint16_t totalWatts, uint8_t inverterCount);
int32_t applyDemandLimits(int32_t target, uint16_t maxPower, bool nightMode,
                          uint16_t nightMaxPower);
bool sumSelectedShellyPhases(bool phaseASelected, bool phaseAValid, float phaseA,
                             bool phaseBSelected, bool phaseBValid, float phaseB,
                             bool phaseCSelected, bool phaseCValid, float phaseC,
                             float &total);
void buildDemandFrame(uint16_t watts, uint8_t frame[8]);
