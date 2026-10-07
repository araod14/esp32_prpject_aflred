# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

An ESP32 firmware (PlatformIO + Arduino framework) that runs a WiFi HTTP server exposing
an API-key-protected REST API, with a WiFi manager and outbound telemetry push. All
firmware logic lives in `src/main.cpp`. A companion Node/Express web app lives in
`server/` (the VPS monitoring dashboard — its own README and deps). `include/` holds
`secrets.h` (git-ignored) and its template `secrets.example.h`.

## Commands

Build/upload/monitor target the `esp32doit-devkit-v1` environment defined in `platformio.ini`.

- Build: `platformio run`
- Upload to board: `platformio run -t upload -e esp32doit-devkit-v1`
- Serial monitor: `platformio device monitor -b 115200`
- Find USB port: `ls /dev/ttyUSB* 2>/dev/null || ls /dev/ttyACM*`

There is no test suite or linter configured.

## Architecture

Standard Arduino `setup()` / `loop()` lifecycle:

- `setup()` loads WiFi credentials, tries to connect (blocking, up to 20 retries), and
  branches: on success it registers the REST routes (`startStaServer()`); on failure it
  starts a captive-portal Access Point (`startApPortal()`). `loop()` calls
  `server.handleClient()`, plus `dnsServer.processNextRequest()` when `apMode` is true.
- STA routes: `GET /` (plain HTML help page, no auth), `GET /api/hello`,
  `GET /api/temperature`, `GET /api/output`, `GET /api/wifi/status`,
  `POST /api/wifi/config`, `POST /api/wifi/reset`. All `/api/*` routes call `checkApiKey()`
  first and early-return on failure.
- **Custom request headers must be pre-registered** via `server.collectHeaders()` in
  `setup()` — the Arduino `WebServer` drops any header not listed there. When adding an
  endpoint that reads a new header, extend the `headerKeys[]` array too.
- Auth (`checkApiKey`): accepts the key in `X-API-Key`, `x-api-key`, or `Authorization`
  (bare value or `Bearer <key>`). Returns 401 JSON on missing/invalid key.
- Temperature (`readTemperature`): NTC thermistor on GPIO 34 read via the Steinhart-Hart /
  Beta equation. **Use ADC1 pins (GPIO 32–39) only** — ADC2 pins (e.g. GPIO 4) do not work
  while WiFi is active on the ESP32.
- Output (`handleOutput`): controllable digital output on GPIO 26. `?state=on|off` sets it,
  no arg returns current state.

## WiFi manager

- **Credentials persist in NVS** via the `Preferences` library (namespace `"wifi"`, keys
  `ssid`/`pass`). `loadCredentials()` returns stored values, falling back to the hardcoded
  `ssid`/`password` constants only when NVS is empty (first boot). `saveCredentials()` /
  `clearCredentials()` write/erase them. Changing WiFi no longer needs a recompile.
- **Captive-portal fallback**: when `connectToWifi()` fails, `startApPortal()` brings up a
  SoftAP `ESP32-Setup` (open by default; set `AP_PASSWORD` — min 8 chars — to protect it)
  in `WIFI_AP_STA` mode (AP_STA so `WiFi.scanNetworks()` works), starts a `DNSServer` that
  redirects all hosts to `192.168.4.1`, and serves a config page (`GET /` + `POST /save`,
  no auth — physical AP access assumed). Saving reboots via `ESP.restart()`.
- `POST /api/wifi/config` (`ssid`/`password` form args) and `POST /api/wifi/reset` are the
  API-key-protected equivalents; both persist to NVS and `ESP.restart()`.
- `Preferences` and `DNSServer` ship with the arduino-esp32 framework — no `lib_deps`
  changes needed.

## Robustez (self-healing)

Three unattended-recovery layers live in `loop()`/`setup()`, tuned by `#define`s at the
top of `src/main.cpp`. Any long-running work added to `loop()` must keep feeding the
watchdog (see the `pushTelemetry()` `esp_task_wdt_reset()` call before the blocking TLS POST):

- **Task watchdog** (`esp_task_wdt`, `WDT_TIMEOUT_S` = 15 s): `setup()` reinitializes the
  default TWDT with `panic=true` and subscribes the loop task; `loop()` calls
  `esp_task_wdt_reset()` every iteration. If `loop()` ever stalls past the timeout the chip
  reboots.
- **STA WiFi watchdog**: every `WIFI_CHECK_INTERVAL_MS` (10 s) `loop()` checks
  `WiFi.status()`; on a dropped link it calls `WiFi.reconnect()`, and if WiFi stays down for
  `WIFI_DOWN_REBOOT_MS` (3 min) it reboots to re-run the connect→portal decision.
  `WiFi.setAutoReconnect(true)` is also set at connect time.
- **AP portal auto-exit**: if stuck in the captive portal for `AP_PORTAL_REBOOT_MS` (5 min)
  — e.g. the home network was only momentarily down at boot — it reboots to retry the saved
  credentials instead of waiting for a human.

## Telemetría push + app del VPS (`server/`)

- **Firmware side (`pushTelemetry()`):** a `millis()` timer in `loop()` fires every
  `PUSH_INTERVAL_MS` (default 10 s, skipped while `apMode`). It builds a small JSON with
  `readTemperature()`, the `OUTPUT_PIN` state, `WiFi.SSID/RSSI/localIP`, and `millis()`,
  then `POST`s it to `VPS_INGEST_URL` over `WiFiClientSecure` + `HTTPClient` with the
  `X-Device-Token` header. The response carries the desired output state, parsed by
  `parseDesiredOutput()` (minimal string parse, no ArduinoJson) and applied via
  `applyDesiredOutput()`. This is the remote-control path: the VPS never reaches the ESP32,
  the ESP32 pulls commands on each push (control latency ≈ one interval).
- **TLS toggle:** `VPS_TLS_INSECURE` (default `1`) uses `client.setInsecure()` for instant
  bring-up; set `0` to validate against `VPS_ROOT_CA` (paste Let's Encrypt ISRG Root X1).
- **Config:** `VPS_INGEST_URL` and `DEVICE_TOKEN` come from `include/secrets.h`;
  `PUSH_INTERVAL_MS` is in `src/main.cpp`.
- **VPS app (`server/`):** Node/Express + `better-sqlite3`. `POST /api/ingest`
  (`X-Device-Token` auth) stores readings and returns the desired output; the dashboard
  routes (`/`, `/api/state`, `/api/history`, `/api/command`) use HTTP Basic Auth. The
  desired-output command is a single persistent row (desired-state model, idempotent).
  Frontend chart is native canvas (no CDN/deps). Deploy via nginx + certbot + systemd
  (`server/deploy/`). See `server/README.md`. Run/test locally with `npm install && npm
  start` — do **not** run the Node app from firmware build/upload flows.

## Configuration

These are hardcoded constants at the top of `src/main.cpp` (not env/build flags):
`ssid`, `password`, `API_KEY`, `AP_SSID`/`AP_PASSWORD`, and the thermistor `#define`s
(series resistance, beta coefficient, nominal temp/resistance). **Secrets** (WiFi
ssid/password, `API_KEY`, `VPS_INGEST_URL`, `DEVICE_TOKEN`) come from `include/secrets.h`
(git-ignored — the repo is public); copy `include/secrets.example.h` to create it. Changing most of these
requires a recompile + upload. **Exception:** `ssid`/`password` are only first-boot
defaults — at runtime WiFi credentials live in NVS and are changed via the captive portal
or `POST /api/wifi/config` (see WiFi manager), no recompile needed.

## Remote access

The device is exposed to the internet via ngrok pointed at the LAN IP + port 80
(`ngrok http <esp32-ip>:80`); see README.md for the full ngrok setup. The firmware itself
is unaware of ngrok.
