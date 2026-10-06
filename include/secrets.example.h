// Secretos del firmware. Plantilla: copia a include/secrets.h y rellena los valores.
#pragma once

// Credenciales WiFi por defecto (se usan solo si NVS está vacío).
#define SECRET_WIFI_SSID     "TU_SSID"
#define SECRET_WIFI_PASSWORD "TU_PASSWORD"

// API Key de la REST local del ESP32
#define SECRET_API_KEY       "CAMBIA_ESTA_API_KEY"

// Telemetría push al VPS
#define SECRET_VPS_INGEST_URL "https://esp32.tudominio.com/api/ingest"
#define SECRET_DEVICE_TOKEN   "CAMBIA_ESTE_TOKEN"   // = DEVICE_TOKEN del .env del VPS
