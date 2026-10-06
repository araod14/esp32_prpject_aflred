# ESP32 Monitor (app del VPS)

Dashboard + backend para monitorear el ESP32 de forma remota mediante **telemetría push**:
el ESP32 abre una conexión saliente HTTPS al VPS cada pocos segundos, envía su estado
(temperatura, salida, WiFi) y recibe en la misma respuesta el estado deseado de la salida.
El VPS **no** necesita alcanzar al ESP32, así que funciona detrás de NAT sin túnel ni
equipo siempre encendido en la LAN.

```
ESP32 (LAN) --HTTPS POST /api/ingest--> VPS (Node/Express + SQLite) --> Dashboard
```

## Endpoints

| Método | Ruta | Auth | Descripción |
|--------|------|------|-------------|
| `POST` | `/api/ingest` | `X-Device-Token` | El ESP32 envía telemetría; responde `{"output":"on"\|"off"\|null}` |
| `GET` | `/api/state` | Basic Auth | Última lectura + online/offline + estado deseado |
| `GET` | `/api/history?minutes=60` | Basic Auth | Serie temporal para el chart |
| `POST` | `/api/command` | Basic Auth | Fija el estado deseado de la salida (`{"output":"on"\|"off"\|null}`) |
| `GET` | `/` | Basic Auth | Dashboard |

## Ejecutar en local (desarrollo)

```bash
cd server
npm install
cp .env.example .env      # edita DEVICE_TOKEN, DASH_USER, DASH_PASS
npm start                 # http://localhost:8080
```

Prueba la ingesta:

```bash
curl -X POST http://localhost:8080/api/ingest \
  -H "X-Device-Token: TU_TOKEN" -H "Content-Type: application/json" \
  -d '{"temp_c":25.3,"output":"off","rssi":-50,"ssid":"MiRed","ip":"192.168.1.50","uptime_ms":60000}'
# -> {"output":null}
```

Forzar la salida desde el dashboard (o por API):

```bash
curl -X POST http://localhost:8080/api/command \
  -u admin:TU_CLAVE -H "Content-Type: application/json" -d '{"output":"on"}'
# el próximo /api/ingest devolverá {"output":"on"}
```

## Despliegue en el VPS

Requisitos: Node ≥ 18, nginx, certbot, un dominio apuntando al VPS (`esp32.tudominio.com`).

```bash
# 1) Código en el VPS
sudo mkdir -p /opt/esp32-monitor && sudo chown $USER /opt/esp32-monitor
# (copia/clona el repo aquí; queda en /opt/esp32-monitor/server)
cd /opt/esp32-monitor/server
npm ci --omit=dev
cp .env.example .env         # edita secretos; DEVICE_TOKEN = openssl rand -hex 32

# 2) Servicio systemd
sudo cp deploy/esp32-monitor.service /etc/systemd/system/
#   revisa User= y WorkingDirectory= dentro del .service
sudo systemctl daemon-reload
sudo systemctl enable --now esp32-monitor
sudo systemctl status esp32-monitor

# 3) nginx + TLS
sudo cp deploy/nginx-esp32.conf /etc/nginx/sites-available/esp32
sudo ln -s /etc/nginx/sites-available/esp32 /etc/nginx/sites-enabled/
#   ajusta server_name en el archivo
sudo nginx -t && sudo systemctl reload nginx
sudo certbot --nginx -d esp32.tudominio.com     # emite el cert Let's Encrypt
```

Tras esto, el dashboard queda en `https://esp32.tudominio.com` (Basic Auth) y el ESP32
debe apuntar `VPS_INGEST_URL` a `https://esp32.tudominio.com/api/ingest`.

## Notas

- **SQLite** (`better-sqlite3`) guarda el histórico en `data.db` (o `DB_PATH`). Se poda
  según `RETENTION_DAYS`.
- El `DEVICE_TOKEN` protege la ingesta; la `API_KEY` de la REST local del ESP32 es
  independiente. Usa valores fuertes en ambos.
- El chart es canvas nativo (sin dependencias de frontend / sin CDN).
