# Agent instructions

## Project orientation

- This is an ESP8266 / NodeMCU v2 Arduino firmware built with PlatformIO,
  using LittleFS. See [README.md](README.md) for hardware, setup and behavior,
  and [platformio.ini](platformio.ini) for environments and dependencies.
- [src/main.cpp](src/main.cpp) owns setup, the control loop and watchdog.
  [src/shelly.cpp](src/shelly.cpp) handles measurement polling and fallback;
  [src/rs485.cpp](src/rs485.cpp) handles inverter communication.
- Configuration fields live in [src/config.h](src/config.h); defaults,
  validation and persistence are in [src/storage.cpp](src/storage.cpp).
  Keep these consistent with the configuration form and status API in
  [src/http_server.cpp](src/http_server.cpp), MQTT in
  [src/mqtt_mgr.cpp](src/mqtt_mgr.cpp), and user help in
  [src/help_page.h](src/help_page.h).

## Scope and device safety

- Preserve the existing repository layout. Do not add, remove, rename, or move
  project files or directories unless the user explicitly requests it.
- Ask before creating tests or test infrastructure.
- Inspect git status and relevant diffs before editing. Preserve existing
  local work; do not revert unrelated changes.
- Do not change firmware behavior, controller settings, or configuration unless
  the user explicitly requests the change.
- Do not flash firmware, upload over OTA, or make changes to a running
  controller without explicit permission. Builds and tests are not permission
  to upload, reboot, reset or write configuration on a device.
- Keep credentials, device configuration backups and other secrets out of the
  repository and logs. Review staged files before committing.

## Firmware conventions

- `g_demand`, RS485 demand, `max_power` and `night_max_power` are per inverter;
  `static_watt` and `fallback_watt` are system totals. MQTT `demand` is total;
  `demand_per_inverter` and HTTP status `demand` are per inverter.
  A requested setpoint is not measured delivered power.
- Keep the main loop responsive: do not introduce blocking network polling.
  Parse asynchronous HTTP responses in loop context, not network callbacks.
  UART `Serial` is reserved for RS485; use the existing Telnet logging.
- The optional export meter is monitoring only. Its failures must not change
  regulation or fallback. It shares the consumption meter's polling interval.
- Distinguish a valid zero measurement from missing/invalid data. Unavailable
  phase telemetry is JSON `null`, not a fabricated zero.
- Preserve OTA polling/RS485 pause guards and power limits when changing
  affected code. Use the existing helpers in
  [src/safety_logic.cpp](src/safety_logic.cpp) rather than duplicating them.
- Treat [the KlausLi algorithm reference](docs/REGELALGORITHMUS_Original_v1.0.3.3.md)
  as historical reverse-engineering evidence, not a verified specification of
  this firmware. Preserve its uncertainties; do not restore original behavior
  solely to match it. Its fork comparison describes an earlier snapshot.

## Build and validation

- Build firmware with `pio run` (default environment: `nodemcuv2`).
  If `pio` is not on PATH, use the existing PlatformIO installation via its
  executable or `python -m platformio run` with the correct interpreter.
  Do not use upload targets unless explicitly requested.
- Run existing hardware-independent tests with `pio test -e native`.
  They require a native GCC/MinGW-w64 C++ compiler on PATH and cover the
  safety helpers, not the full firmware or ESP8266 runtime.
- Respect explicit requests not to compile. For code changes otherwise run
  the relevant existing tests and firmware build; check `git diff --check`.
  Documentation-only changes do not require a firmware build.
- Report failed or skipped validation and its reason; never equate a build
  or native tests with hardware, timing, heap or long-term stability proof.
- Build output is `.pio/build/nodemcuv2/firmware.bin`. Verify that it was built
  from the requested source state before describing it as current; a commit
  or push does not rebuild it. Do not commit build artifacts.
- Before large toolchain installations check free disk space. Reuse installed
  tools and declared dependencies instead of adding unnecessary tooling.
