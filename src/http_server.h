#pragma once

#include <ESP8266WebServer.h>

extern ESP8266WebServer server;

struct LoopTimingStats {
    uint32_t averageUs = 0;
    uint32_t maximumUs = 0;
    uint32_t peakUs = 0;
    bool ready = false;
};

extern LoopTimingStats g_loopTiming;

void httpServerBegin();
void httpServerLoop();
