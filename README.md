# Servidor Web ESP32 con API

Un servidor web en ESP32 con autenticación por API Key. Para acceso remoto incluye
**telemetría push a un VPS** con dashboard (recomendado, ver [más abajo](#monitoreo-remoto-push-al-vps--recomendado))
o, como alternativa, exposición directa vía ngrok.

## Configuración Inicial

### 1. Instala PlatformIO
```bash
pip install platformio
```

### 2. Configura WiFi
El ESP32 incluye un **WiFi Manager**: guarda las credenciales en memoria no volátil
(NVS) y arranca un portal de configuración si no logra conectar. Ya **no es obligatorio**
editar el código para cambiar de red (ver [WiFi Manager](#wifi-manager)).

Los valores de `src/main.cpp` solo se usan como red por defecto en el **primer arranque**
(cuando NVS está vacío):
```cpp
const char* ssid = "TU_SSID";
const char* password = "TU_PASSWORD";
```

### 3. (Opcional) Cambia la API Key
En `src/main.cpp`:
```cpp
const char* API_KEY = "mi_api_key_secreta";
```

### 4. Compila y sube al ESP32
```bash
platformio run -t upload -e esp32doit-devkit-v1
```

## Monitoreo del ESP32

### Ver logs en tiempo real
```bash
platformio device monitor -b 115200
```

**Verás output como:**
```
--- Terminal on /dev/ttyUSB0 | 115200 8-N-1
--- Available filters and text transformations: debug, default, direct, esp32_exception_decoder, hexlify, log2file, nocontrol, printable, send_on_enter, time
--- Quit: Ctrl+C | Menu: Ctrl+T | Help: Ctrl+T followed by Ctrl+H

ets Jul 29 2019 12:21:46
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)

Iniciando ESP32...
Conectando a WiFi: Redmi Note 11
...
✓ WiFi conectado!
IP: 192.168.1.58
Servidor web iniciado en puerto 80
Accede a http://192.168.1.58 en tu navegador
Headers recibidos: Authorization: mi_api_key_secreta
```

### Comandos útiles en el monitor

| Atajo | Función |
|-------|---------|
| `Ctrl+C` | Salir del monitor |
| `Ctrl+T` | Abre menú de opciones |
| `Ctrl+T` + `h` | Ver ayuda |
| `Ctrl+T` + `l` | Limpiar pantalla |

### Reiniciar ESP32 desde el monitor
1. Presiona `Ctrl+T` (abre menú)
2. Presiona `r` (reiniciar)

O presiona el botón RESET en el ESP32

### Filtrar logs
```bash
# Ver solo errores
platformio device monitor -b 115200 | grep -i error

# Ver logs sin timestamps
platformio device monitor -b 115200 --filter default
```

### Detener monitoreo
Presiona `Ctrl+C`

### Detectar puerto USB automáticamente
```bash
# Ver puertos disponibles
ls /dev/ttyUSB* 2>/dev/null || ls /dev/ttyACM* 2>/dev/null

# Debería mostrar algo como: /dev/ttyUSB0
```

## Acceso Local (dentro de tu red)

### Endpoints disponibles

Todos los endpoints `/api/*` requieren la API Key (header `X-API-Key`, `x-api-key` o
`Authorization`, con o sin prefijo `Bearer `).

| Método | Ruta | Descripción |
|--------|------|-------------|
| `GET` | `/api/hello` | Devuelve `{"mensaje":"hola mundo"}` |
| `GET` | `/api/temperature` | Lee el termistor NTC (GPIO 34): `{"temperatura_c":25.31}` |
| `GET` | `/api/output` | Estado de la salida digital (GPIO 26). Con `?state=on\|off` la cambia |
| `GET` | `/api/wifi/status` | Estado de conexión (ver [WiFi Manager](#wifi-manager)) |
| `POST` | `/api/wifi/config` | Guarda una red nueva (`ssid`, `password`) y reinicia |
| `POST` | `/api/wifi/reset` | Borra las credenciales y reinicia al portal |

### Probar con curl (Linux/Mac)
```bash
curl -H "X-API-Key: mi_api_key_secreta" http://192.168.x.x/api/hello
# {"mensaje":"hola mundo"}

curl -H "X-API-Key: mi_api_key_secreta" http://192.168.x.x/api/temperature
# {"temperatura_c":25.31}

# Encender la salida (GPIO 26)
curl -H "X-API-Key: mi_api_key_secreta" "http://192.168.x.x/api/output?state=on"
# {"pin":26,"state":"on"}
```

### Acceso por navegador
Ve a `http://192.168.x.x` para ver instrucciones.

## WiFi Manager

El firmware gestiona las credenciales WiFi sin necesidad de recompilar:

- **Persistencia (NVS):** las credenciales se guardan en memoria no volátil y sobreviven
  reinicios y recompilaciones. Los `ssid`/`password` de `src/main.cpp` solo se usan como
  red por defecto si NVS está vacío (primer arranque).
- **Fallback a portal cautivo:** si el ESP32 no logra conectar (red inexistente, clave
  cambiada, otra ubicación), arranca su propio Access Point **`ESP32-Setup`** (abierto).

### Consultar el estado
```bash
curl -H "X-API-Key: mi_api_key_secreta" http://192.168.x.x/api/wifi/status
```
```json
{"connected":true,"mode":"sta","ssid":"MiRed","ip":"192.168.100.111","rssi":-50}
```

### Agregar / cambiar de red por API (estando conectado)
```bash
curl -X POST -H "X-API-Key: mi_api_key_secreta" \
  -d "ssid=MiRed&password=miclave" http://192.168.x.x/api/wifi/config
```
El ESP32 guarda la red y se reinicia para conectarse a ella.

### Agregar una red por el portal cautivo (sin cable)
Úsalo cuando el ESP32 no esté conectado a ninguna red:
1. Con el móvil o PC, conéctate a la red WiFi **`ESP32-Setup`**.
2. Se abre automáticamente la página de configuración (o ve a `http://192.168.4.1`).
3. Elige tu red de la lista, escribe la contraseña y pulsa **Guardar y conectar**.
4. El ESP32 se reinicia y se conecta a la red elegida.

### Volver al portal (olvidar la red guardada)
```bash
curl -X POST -H "X-API-Key: mi_api_key_secreta" http://192.168.x.x/api/wifi/reset
```

> **Nota:** el AP `ESP32-Setup` está **abierto** por defecto. Para protegerlo con
> contraseña, descomenta `AP_PASSWORD` en `src/main.cpp` (mínimo 8 caracteres).

## Monitoreo remoto (push al VPS) — recomendado

En vez de exponer el ESP32 con un túnel, el firmware puede **enviar su telemetría de
forma saliente** a una app alojada en tu VPS. El ESP32 hace `POST` HTTPS cada 10 s con su
estado (temperatura, salida, WiFi) y recibe en la respuesta el comando pendiente para la
salida. No requiere túnel ni un equipo siempre encendido en la LAN, y funciona detrás de
NAT.

```
ESP32 (LAN) --HTTPS POST /api/ingest--> VPS (Node/Express + SQLite) --> Dashboard web
```

**App del VPS**: código y guía de despliegue completa (nginx + Let's Encrypt + systemd) en
[`server/README.md`](server/README.md).

**Configurar el firmware** (constantes en `src/main.cpp`, sección "Telemetría push al VPS"):

```cpp
const char* VPS_INGEST_URL = "https://esp32.tudominio.com/api/ingest";
const char* DEVICE_TOKEN   = "CAMBIA_ESTE_TOKEN";   // = DEVICE_TOKEN del .env del VPS
#define PUSH_INTERVAL_MS 10000                        // cada 10 s
#define VPS_TLS_INSECURE 1   // 1 = no valida el cert (rápido); 0 = valida con VPS_ROOT_CA
```

> **Seguridad TLS**: `VPS_TLS_INSECURE 1` conecta de inmediato pero **no valida** el
> certificado del VPS. Para validarlo, pon `0` y pega la CA raíz *ISRG Root X1* de Let's
> Encrypt en `VPS_ROOT_CA` (instrucciones en el propio archivo).

Luego recompila y sube:

```bash
platformio run -t upload -e esp32doit-devkit-v1 --upload-port /dev/ttyUSB4
```

El dashboard queda en `https://esp32.tudominio.com` (protegido con usuario/clave).

## Desplegar con ngrok (alternativa: acceso directo a la API local)

### 1. Instala ngrok
**Linux (Fedora):**
```bash
cd /tmp && wget https://bin.equinox.io/c/bNyj1mQVY4c/ngrok-v3-stable-linux-amd64.tar.gz -O ngrok.tar.gz && tar -xzf ngrok.tar.gz && sudo install -m 0755 ngrok /usr/local/bin/ngrok
```

**macOS:**
```bash
brew install ngrok
```

**Windows:** Descarga desde https://ngrok.com/download

### 2. Crea cuenta en ngrok
Ve a https://ngrok.com/sign-up y obtén tu Auth Token

### 3. Configura el token
```bash
ngrok config add-authtoken TU_AUTH_TOKEN
```

### 4. Inicia ngrok
```bash
ngrok http 192.168.1.58:80
```

Verás algo como:
```
Session Status        online
Forwarding            https://abc123def456.ngrok-free.dev -> http://192.168.1.58:80
```

### 5. Usa tu API desde cualquier lugar

**Linux/Mac:**
```bash
curl -H "Authorization: mi_api_key_secreta" https://abc123def456.ngrok-free.dev/api/hello
```

**PowerShell (Windows):**
```powershell
curl -H "Authorization: mi_api_key_secreta" https://abc123def456.ngrok-free.dev/api/hello
```

**JavaScript (navegador):**
```javascript
fetch('https://abc123def456.ngrok-free.dev/api/hello', {
  headers: { 'Authorization': 'mi_api_key_secreta' }
})
.then(r => r.json())
.then(data => console.log(data))
```

**Postman:**
1. GET a `https://abc123def456.ngrok-free.dev/api/hello`
2. Header: `Authorization: mi_api_key_secreta`
3. Send

## Workflow Completo

### Terminal 1: Monitorea el ESP32
```bash
platformio device monitor -b 115200
```

### Terminal 2: Ejecuta ngrok
```bash
ngrok http 192.168.1.58:80
```

### Terminal 3: Prueba la API
```bash
curl -H "Authorization: mi_api_key_secreta" https://tu-url.ngrok-free.dev/api/hello
```

## Troubleshooting

| Problema | Solución |
|----------|----------|
| ESP32 no conecta a WiFi | Conéctate al AP `ESP32-Setup` y reconfigura (ver [WiFi Manager](#wifi-manager)) |
| Puerto USB no detectado | `ls /dev/ttyUSB*` o instala drivers CH340 |
| ngrok dice "Permission denied" | Usa `sudo` o agrega usuario al grupo dialout |
| API Key inválida | Verifica que envíes en header `Authorization` |
| ngrok URL cambia | Plan gratuito cambia URLs. Usa plan Pro para fijas |

## Notas Importantes

- **ngrok debe estar corriendo** para que la URL pública funcione
- La URL gratuita de ngrok **cambia cada reinicio** (plan Pro la fija)
- **Mantén tu Auth Token seguro**, no lo compartas
- Asegúrate que el ESP32 tenga **buena conexión WiFi**
- El servidor ESP32 es **muy ligero**, puede soportar muchas conexiones

## API Key
Por defecto: `mi_api_key_secreta`

Cambiar en `src/main.cpp`:
```cpp
const char* API_KEY = "tu_nueva_clave_segura";
```

Luego recompila y sube al ESP32.
