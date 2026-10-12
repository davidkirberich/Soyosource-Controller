#include "safety_logic.h"

int32_t distributeTotalDemand(uint16_t totalWatts, uint8_t inverterCount) {
    return inverterCount == 0 ? 0 : totalWatts / inverterCount;
}

int32_t applyDemandLimits(int32_t target, uint16_t maxPower, bool nightMode,
                          uint16_t nightMaxPower) {
    if (target < 0) target = 0;
    if (target > maxPower) target = maxPower;
    if (nightMode && target > nightMaxPower) target = nightMaxPower;
    return target;
}

bool sumSelectedShellyPhases(bool phaseASelected, bool phaseAValid, float phaseA,
                             bool phaseBSelected, bool phaseBValid, float phaseB,
                             bool phaseCSelected, bool phaseCValid, float phaseC,
                             float &total) {
    if ((!phaseASelected && !phaseBSelected && !phaseCSelected) ||
        (phaseASelected && !phaseAValid) ||
        (phaseBSelected && !phaseBValid) ||
        (phaseCSelected && !phaseCValid)) {
        return false;
    }

    total = (phaseASelected ? phaseA : 0.0f) +
            (phaseBSelected ? phaseB : 0.0f) +
            (phaseCSelected ? phaseC : 0.0f);
    return true;
}

void buildDemandFrame(uint16_t watts, uint8_t frame[8]) {
    frame[0] = 0x24;
    frame[1] = 0x56;
    frame[2] = 0x00;
    frame[3] = 0x21;
    frame[4] = (watts >> 8) & 0xFF;
    frame[5] = watts & 0xFF;
    frame[6] = 0x80;
    frame[7] = (uint8_t)((264 - frame[4] - frame[5]) & 0xFF);
}
