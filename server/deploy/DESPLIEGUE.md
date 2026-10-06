# Despliegue de la app en el VPS (Ubuntu/Debian)

Guía paso a paso para subir el código al VPS y dejar el dashboard corriendo en
`https://esp32.tudominio.com` con TLS, como servicio systemd detrás de nginx.

> Reemplaza en todos los comandos:
> - `esp32.tudominio.com` → tu subdominio real
> - `<TU_REPO_GIT>` → la URL del repositorio (p. ej. `https://github.com/usuario/sp32.git`)
> - Ejecuta como un usuario con `sudo` (no como root directo, salvo que se indique).

Arquitectura del despliegue:

```
ESP32 (LAN) --HTTPS POST /api/ingest--> nginx :443 (TLS) --proxy--> Node :8080 (systemd) --> SQLite
                                         navegador --HTTPS--> dashboard (Basic Auth)
```

---

## 0. Requisitos previos

- VPS Ubuntu/Debian con acceso SSH y usuario con `sudo`.
- **DNS**: un registro **A** de `esp32.tudominio.com` apuntando a la IP pública del VPS.
  Verifícalo antes de pedir el certificado:

  ```bash
  dig +short esp32.tudominio.com      # debe devolver la IP del VPS
  ```

- Puertos 80 y 443 abiertos (ver paso 5, firewall).

---

## 1. Instalar dependencias del sistema

```bash
sudo apt update
sudo apt install -y git nginx curl

# Node.js 20 LTS desde NodeSource (el proyecto exige Node >= 18)
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs

# Verifica
node -v        # v20.x
which node     # anota esta ruta: debe ser /usr/bin/node (la usa el .service)
```

> **Importante:** el archivo `esp32-monitor.service` arranca con `ExecStart=/usr/bin/node`.
> Si `which node` devuelve otra ruta (p. ej. instalaste con nvm), edita esa línea del
> `.service` en el paso 4 para que apunte a la ruta correcta. NodeSource sí instala en
> `/usr/bin/node`.

`better-sqlite3` trae binarios precompilados para Node 20; normalmente **no** necesitas
compilar. Si en el paso 3 fallara la instalación nativa, instala las herramientas de build:
`sudo apt install -y build-essential python3`.

---

## 2. Subir el código (git clone)

El servicio y nginx esperan el proyecto en `/opt/esp32-monitor` (la app queda en
`/opt/esp32-monitor/server`).

```bash
sudo mkdir -p /opt/esp32-monitor
sudo chown "$USER":"$USER" /opt/esp32-monitor
git clone <TU_REPO_GIT> /opt/esp32-monitor
```

Si tu repo tiene el `server/` en la raíz (como este proyecto), tras el clone debe existir
`/opt/esp32-monitor/server/package.json`. Compruébalo:

```bash
ls /opt/esp32-monitor/server/package.json    # debe existir
```

---

## 3. Instalar dependencias de Node y configurar secretos

```bash
cd /opt/esp32-monitor/server
npm ci --omit=dev          # instalación limpia solo de dependencias de producción
```

> Si el repo no incluye `package-lock.json`, usa `npm install --omit=dev`.

Crea el `.env` a partir del ejemplo y **cambia los secretos**:

```bash
cp .env.example .env

# Genera un token fuerte para el ESP32 y cópialo (lo necesitarás en el firmware):
openssl rand -hex 32
```

Edita `.env` (`nano .env`) y ajusta como mínimo:

```ini
PORT=8080
DEVICE_TOKEN=<pega-aqui-el-token-generado>   # == DEVICE_TOKEN del firmware
DASH_USER=admin
DASH_PASS=<una-clave-fuerte-para-el-dashboard>
OFFLINE_AFTER_MS=30000
RETENTION_DAYS=7
# DB_PATH=/var/lib/esp32-monitor/data.db      # opcional; ver nota de SQLite abajo
```

Node aborta al arrancar si `DEVICE_TOKEN` o `DASH_PASS` están vacíos, así que no dejes los
valores de ejemplo.

---

## 4. Servicio systemd

El servicio corre como `www-data`. Hay que darle permiso de **escritura** sobre la carpeta
donde SQLite crea `data.db` (y sus archivos `-wal`/`-shm`). Por defecto ese es el propio
`server/`:

```bash
# El dueño del código puede seguir siendo tu usuario, pero www-data debe poder escribir
# el archivo SQLite. Lo más simple: que www-data sea dueño de la carpeta server/.
sudo chown -R www-data:www-data /opt/esp32-monitor/server

# Protege el .env (contiene el token y la clave del dashboard)
sudo chmod 600 /opt/esp32-monitor/server/.env
sudo chown www-data:www-data /opt/esp32-monitor/server/.env
```

Instala la unidad:

```bash
cd /opt/esp32-monitor/server
sudo cp deploy/esp32-monitor.service /etc/systemd/system/

# Revisa que User=, WorkingDirectory= y ExecStart= (ruta de node) sean correctos:
sudoedit /etc/systemd/system/esp32-monitor.service

sudo systemctl daemon-reload
sudo systemctl enable --now esp32-monitor
sudo systemctl status esp32-monitor --no-pager
```

Deberías ver `active (running)`. Mira los logs si algo falla:

```bash
journalctl -u esp32-monitor -n 50 --no-pager
```

Prueba que Node responde localmente (aún sin nginx):

```bash
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8080/api/state
# 401 es correcto aquí: el endpoint pide Basic Auth. Significa que Node vive.
```

---

## 5. Firewall (si usas ufw)

```bash
sudo ufw allow OpenSSH
sudo ufw allow 'Nginx Full'     # abre 80 y 443
sudo ufw enable                 # si aún no estaba activo
sudo ufw status
```

---

## 6. nginx + TLS (Let's Encrypt)

```bash
cd /opt/esp32-monitor/server

# 1) Copia el vhost y pon tu dominio real en server_name
sudo cp deploy/nginx-esp32.conf /etc/nginx/sites-available/esp32
sudo sed -i 's/esp32\.tudominio\.com/TU_DOMINIO_REAL/g' /etc/nginx/sites-available/esp32
#   (reemplaza TU_DOMINIO_REAL arriba, o edítalo a mano:
#    sudoedit /etc/nginx/sites-available/esp32  -> cambia server_name)

# 2) Habilítalo
sudo ln -s /etc/nginx/sites-available/esp32 /etc/nginx/sites-enabled/
# (opcional) quita el sitio por defecto si estorba:
# sudo rm -f /etc/nginx/sites-enabled/default

# 3) Valida y recarga
sudo nginx -t && sudo systemctl reload nginx
```

En este punto `http://esp32.tudominio.com` ya debería pedir usuario/clave (Basic Auth).

Ahora emite el certificado. certbot reescribe el vhost para añadir el bloque `443` + la
redirección de 80→443:

```bash
sudo apt install -y certbot python3-certbot-nginx
sudo certbot --nginx -d esp32.tudominio.com
# elige redirigir HTTP a HTTPS cuando lo pregunte

# La renovación automática ya queda programada; puedes probarla en seco:
sudo certbot renew --dry-run
```

---

## 7. Verificación de extremo a extremo

```bash
# Dashboard por HTTPS (debe devolver 200 con credenciales correctas)
curl -s -o /dev/null -w "%{http_code}\n" -u admin:TU_CLAVE https://esp32.tudominio.com/api/state

# Simula una ingesta del ESP32 (debe devolver {"output":null} o el comando pendiente)
curl -s -X POST https://esp32.tudominio.com/api/ingest \
  -H "X-Device-Token: TU_DEVICE_TOKEN" -H "Content-Type: application/json" \
  -d '{"temp_c":25.3,"output":"off","rssi":-50,"ssid":"MiRed","ip":"192.168.1.50","uptime_ms":60000}'
```

Abre `https://esp32.tudominio.com` en el navegador → login con `DASH_USER`/`DASH_PASS` →
deberías ver el dashboard (aparecerá OFFLINE hasta que el ESP32 real empiece a reportar).

---

## 8. Configurar el firmware del ESP32

En `src/main.cpp`, antes de compilar y flashear, ajusta las constantes para que apunten al
VPS (ver `CLAUDE.md` → "Telemetría push"):

```cpp
#define VPS_INGEST_URL  "https://esp32.tudominio.com/api/ingest"
#define DEVICE_TOKEN    "el-mismo-token-del-.env-del-VPS"
// PUSH_INTERVAL_MS por defecto 10000 (10 s)
// VPS_TLS_INSECURE: 1 para bring-up rápido; 0 para validar contra VPS_ROOT_CA (ISRG Root X1)
```

El `DEVICE_TOKEN` del firmware **debe ser idéntico** al `DEVICE_TOKEN` del `.env` del VPS.
Luego compila y sube:

```bash
# desde tu máquina de desarrollo, en la raíz del proyecto
platformio run -t upload -e esp32doit-devkit-v1
platformio device monitor -b 115200     # verifica en el log que el POST devuelve 200
```

---

## 9. Actualizaciones futuras (redeploy)

```bash
cd /opt/esp32-monitor
sudo -u www-data git pull          # si www-data es dueño del repo (paso 4)
cd server
sudo -u www-data npm ci --omit=dev # solo si cambiaron dependencias
sudo systemctl restart esp32-monitor
journalctl -u esp32-monitor -n 30 --no-pager
```

> Si al hacer `chown -R www-data` el `git pull` da problemas de "dueño inseguro", puedes
> ejecutar el pull como tu usuario y luego `sudo chown -R www-data:www-data
> /opt/esp32-monitor/server` de nuevo.

---

## Notas y resolución de problemas

- **SQLite / permisos:** el error más común es que el servicio no pueda escribir `data.db`.
  Asegúrate de que `www-data` sea dueño de la carpeta `server/` (paso 4). Si prefieres
  separar los datos del código, define `DB_PATH=/var/lib/esp32-monitor/data.db` en `.env` y:
  ```bash
  sudo mkdir -p /var/lib/esp32-monitor
  sudo chown www-data:www-data /var/lib/esp32-monitor
  ```
  Nota: `esp32-monitor.service` trae `ProtectSystem=full` (deja `/usr`, `/boot`, `/etc` en
  solo lectura). `/opt` y `/var/lib` siguen siendo escribibles, así que ambas rutas de la
  base de datos funcionan.

- **502 Bad Gateway en nginx:** Node no está corriendo o escucha en otro puerto. Revisa
  `systemctl status esp32-monitor` y que `PORT` del `.env` coincida con el `proxy_pass` del
  vhost (`127.0.0.1:8080`).

- **El ESP32 no aparece / sigue OFFLINE:** verifica que `DEVICE_TOKEN` coincide en ambos
  lados, que `VPS_INGEST_URL` usa `https://` y la ruta `/api/ingest`, y mira los logs:
  `journalctl -u esp32-monitor -f`.

- **certbot falla:** casi siempre es DNS (el A no apunta aún al VPS) o el puerto 80 cerrado.
  Reconfirma `dig +short esp32.tudominio.com` y el firewall.

- **better-sqlite3 no instala:** instala `build-essential python3` y reintenta `npm ci`.
