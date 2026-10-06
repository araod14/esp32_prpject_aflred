#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>

// Secretos (WiFi, API key, URL/token del VPS) en include/secrets.h, excluido de git.
// Si no existe, copia include/secrets.example.h a include/secrets.h y rellénalo.
#include "secrets.h"

// Credenciales WiFi por defecto (se usan solo si NVS está vacío).
// Ya no es necesario recompilar para cambiar de red: usa el portal o /api/wifi/config.
const char* ssid = SECRET_WIFI_SSID;
const char* password = SECRET_WIFI_PASSWORD;

// API Key
const char* API_KEY = SECRET_API_KEY;

// Access Point de rescate (portal cautivo)
const char* AP_SSID = "ESP32-Setup";
// const char* AP_PASSWORD = "esp32setup";  // descomenta para proteger el AP (min. 8 chars)
const char* AP_PASSWORD = nullptr;          // nullptr = AP abierto

// Namespace de almacenamiento no volátil (NVS)
#define NVS_NAMESPACE "wifi"

// ------------------- Telemetría push al VPS -------------------
// El ESP32 envía su estado al VPS cada PUSH_INTERVAL_MS y recibe en la respuesta
// el estado deseado de la salida (control remoto sin necesidad de túnel).
const char* VPS_INGEST_URL = SECRET_VPS_INGEST_URL;
const char* DEVICE_TOKEN   = SECRET_DEVICE_TOKEN;
#define PUSH_INTERVAL_MS 10000                      // cada 10 s

// Validación TLS del certificado del VPS.
//   1 = NO valida el certificado (setInsecure). Funciona de inmediato, menos seguro.
//   0 = valida contra VPS_ROOT_CA (pega abajo la CA raíz ISRG Root X1 de Let's Encrypt).
#define VPS_TLS_INSECURE 1

// CA raíz de Let's Encrypt (ISRG Root X1). Necesaria solo si VPS_TLS_INSECURE = 0.
// Obtén el PEM actual con:
//   openssl s_client -connect esp32.tudominio.com:443 -showcerts </dev/null 2>/dev/null \
//     | openssl x509  (o descarga ISRG Root X1 desde https://letsencrypt.org/certs/)
const char* VPS_ROOT_CA = R"CERT(
-----BEGIN CERTIFICATE-----
PEGA_AQUI_LA_CA_ISRG_ROOT_X1_SI_ACTIVAS_LA_VALIDACION_TLS
-----END CERTIFICATE-----
)CERT";

// NTC Thermistor (GPIO 34 / D34, ADC1, divisor con 10k pull-up a 3.3V)
// Nota: usar ADC1 (GPIO 32-39) porque ADC2 (ej. GPIO 4) no funciona con WiFi activo.
#define THERMISTOR_PIN     34
#define SERIES_RESISTOR    10000.0
#define NOMINAL_RESISTANCE 10000.0
#define NOMINAL_TEMP       25.0
#define BETA_COEFFICIENT   3950.0
#define ADC_MAX            4095.0

// Salida digital controlable por API (GPIO 26, pin de propósito general)
#define OUTPUT_PIN         26

// Puerto del servidor
WebServer server(80);

// WiFi manager
Preferences prefs;
DNSServer dnsServer;
bool apMode = false;                       // true si arrancamos en modo portal cautivo
const byte DNS_PORT = 53;
const IPAddress apIP(192, 168, 4, 1);

float readTemperature() {
  int raw = analogRead(THERMISTOR_PIN);
  if (raw <= 0) return NAN;
  float resistance = SERIES_RESISTOR / (ADC_MAX / (float)raw - 1.0);
  float steinhart = resistance / NOMINAL_RESISTANCE;
  steinhart = log(steinhart);
  steinhart /= BETA_COEFFICIENT;
  steinhart += 1.0 / (NOMINAL_TEMP + 273.15);
  steinhart = 1.0 / steinhart;
  return steinhart - 273.15;
}

bool checkApiKey() {
  String apiKey = "";
  if (server.hasHeader("X-API-Key")) {
    apiKey = server.header("X-API-Key");
  } else if (server.hasHeader("x-api-key")) {
    apiKey = server.header("x-api-key");
  } else if (server.hasHeader("Authorization")) {
    apiKey = server.header("Authorization");
  }
  Serial.print("DEBUG - API Key recibido: '");
  Serial.print(apiKey);
  Serial.println("'");
  Serial.print("DEBUG - API Key esperado: '");
  Serial.print(API_KEY);
  Serial.println("'");
  if (apiKey.isEmpty()) {
    server.send(401, "application/json", "{\"error\":\"API Key requerida\"}");
    return false;
  }
  if (apiKey != API_KEY && apiKey != String("Bearer ") + API_KEY) {
    server.send(401, "application/json", "{\"error\":\"API Key inválida\"}");
    return false;
  }
  return true;
}

// ------------------------- Credenciales en NVS -------------------------

// Lee las credenciales guardadas en NVS. Si no hay ninguna, devuelve los
// defaults hardcodeados (preserva el comportamiento previo en el primer arranque).
bool loadCredentials(String &outSsid, String &outPass) {
  prefs.begin(NVS_NAMESPACE, true);  // solo lectura
  String storedSsid = prefs.getString("ssid", "");
  String storedPass = prefs.getString("pass", "");
  prefs.end();

  if (storedSsid.length() > 0) {
    outSsid = storedSsid;
    outPass = storedPass;
    return true;
  }
  // Fallback a los defaults de compilación
  outSsid = ssid;
  outPass = password;
  return false;
}

void saveCredentials(const String &newSsid, const String &newPass) {
  prefs.begin(NVS_NAMESPACE, false);  // lectura/escritura
  prefs.putString("ssid", newSsid);
  prefs.putString("pass", newPass);
  prefs.end();
  Serial.print("Credenciales guardadas en NVS para SSID: ");
  Serial.println(newSsid);
}

void clearCredentials() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  Serial.println("Credenciales borradas de NVS");
}

// ------------------------- Conexión WiFi -------------------------

// Intenta conectar en modo estación. Devuelve true si conecta.
bool connectToWifi(const String &connSsid, const String &connPass) {
  Serial.print("Conectando a WiFi: ");
  Serial.println(connSsid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(connSsid.c_str(), connPass.c_str());

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 20) {
    delay(500);
    Serial.print(".");
    intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("\n✗ No se pudo conectar a WiFi");
  return false;
}

// ------------------------- Handlers API REST -------------------------

void handleRoot() {
  String html = "<h1>Servidor ESP32 activo</h1>";
  html += "<p>Endpoint: GET /api/hello</p>";
  html += "<p>Header requerido: X-API-Key: &lt;tu_api_key&gt;</p>";
  html += "<p>Estado WiFi: GET /api/wifi/status</p>";
  html += "<p>Ejemplo: curl -H 'X-API-Key: &lt;tu_api_key&gt;' http://";
  html += WiFi.localIP().toString();
  html += "/api/hello</p>";
  server.send(200, "text/html", html);
}

void handleHello() {
  if (!checkApiKey()) return;
  server.send(200, "application/json", "{\"mensaje\":\"hola mundo\"}");
}

void handleTemperature() {
  if (!checkApiKey()) return;
  float temp = readTemperature();
  if (isnan(temp)) {
    server.send(500, "application/json", "{\"error\":\"Error leyendo sensor\"}");
    return;
  }
  String json = "{\"temperatura_c\":" + String(temp, 2) + "}";
  server.send(200, "application/json", json);
}

void handleOutput() {
  if (!checkApiKey()) return;

  if (server.hasArg("state")) {
    String state = server.arg("state");
    if (state == "on") {
      digitalWrite(OUTPUT_PIN, HIGH);
    } else if (state == "off") {
      digitalWrite(OUTPUT_PIN, LOW);
    } else {
      server.send(400, "application/json", "{\"error\":\"state debe ser 'on' u 'off'\"}");
      return;
    }
  }

  String current = digitalRead(OUTPUT_PIN) == HIGH ? "on" : "off";
  String json = "{\"pin\":" + String(OUTPUT_PIN) + ",\"state\":\"" + current + "\"}";
  server.send(200, "application/json", json);
}

// GET /api/wifi/status -> estado de la conexión
void handleWifiStatus() {
  if (!checkApiKey()) return;
  bool connected = WiFi.status() == WL_CONNECTED;
  String json = "{";
  json += "\"connected\":" + String(connected ? "true" : "false");
  json += ",\"mode\":\"" + String(apMode ? "ap" : "sta") + "\"";
  json += ",\"ssid\":\"" + WiFi.SSID() + "\"";
  json += ",\"ip\":\"" + (connected ? WiFi.localIP().toString() : String("")) + "\"";
  json += ",\"rssi\":" + String(connected ? WiFi.RSSI() : 0);
  json += "}";
  server.send(200, "application/json", json);
}

// POST /api/wifi/config -> guarda una red nueva y reinicia para conectar
void handleWifiConfig() {
  if (!checkApiKey()) return;
  if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) {
    server.send(400, "application/json", "{\"error\":\"parametro 'ssid' requerido\"}");
    return;
  }
  String newSsid = server.arg("ssid");
  String newPass = server.hasArg("password") ? server.arg("password") : "";
  saveCredentials(newSsid, newPass);
  server.send(200, "application/json",
              "{\"mensaje\":\"Red guardada, reiniciando para conectar\"}");
  delay(500);
  ESP.restart();
}

// POST /api/wifi/reset -> borra credenciales y reinicia al portal cautivo
void handleWifiReset() {
  if (!checkApiKey()) return;
  clearCredentials();
  server.send(200, "application/json",
              "{\"mensaje\":\"Credenciales borradas, reiniciando al modo AP\"}");
  delay(500);
  ESP.restart();
}

void handleNotFound() {
  server.send(404, "application/json", "{\"error\":\"Endpoint no encontrado\"}");
}

// ------------------------- Portal cautivo (modo AP) -------------------------

void handleConfigPortal() {
  int n = WiFi.scanNetworks();
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>ESP32 WiFi Setup</title></head><body>";
  html += "<h2>Configurar WiFi del ESP32</h2>";
  html += "<form method='POST' action='/save'>";
  html += "<label>Red:</label><br><select name='ssid'>";
  if (n <= 0) {
    html += "<option value=''>(no se encontraron redes)</option>";
  } else {
    for (int i = 0; i < n; i++) {
      html += "<option value='" + WiFi.SSID(i) + "'>";
      html += WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)";
      html += "</option>";
    }
  }
  html += "</select><br><br>";
  html += "<label>Contraseña:</label><br>";
  html += "<input type='password' name='password'><br><br>";
  html += "<input type='submit' value='Guardar y conectar'>";
  html += "</form></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) {
    server.send(400, "text/html", "<p>Falta el SSID. <a href='/'>Volver</a></p>");
    return;
  }
  String newSsid = server.arg("ssid");
  String newPass = server.hasArg("password") ? server.arg("password") : "";
  saveCredentials(newSsid, newPass);
  server.send(200, "text/html",
              "<p>Guardado. El ESP32 se reiniciará y se conectará a '" + newSsid +
              "'.</p>");
  delay(1000);
  ESP.restart();
}

void startApPortal() {
  Serial.println("Arrancando portal cautivo en modo AP...");
  apMode = true;

  // AP_STA para poder escanear redes mientras servimos el portal
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  if (AP_PASSWORD != nullptr && strlen(AP_PASSWORD) >= 8) {
    WiFi.softAP(AP_SSID, AP_PASSWORD);
  } else {
    WiFi.softAP(AP_SSID);
  }

  Serial.print("AP \"");
  Serial.print(AP_SSID);
  Serial.print("\" activo. Conéctate y abre http://");
  Serial.println(apIP);

  // DNS captivo: redirige todos los dominios a la IP del AP
  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", handleConfigPortal);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleConfigPortal);  // cualquier ruta abre el portal
  server.begin();
  Serial.println("Portal de configuración iniciado");
}

// ------------------------- Rutas del modo estación -------------------------

void startStaServer() {
  server.on("/", handleRoot);
  server.on("/api/hello", handleHello);
  server.on("/api/temperature", handleTemperature);
  server.on("/api/output", handleOutput);
  server.on("/api/wifi/status", handleWifiStatus);
  server.on("/api/wifi/config", HTTP_POST, handleWifiConfig);
  server.on("/api/wifi/reset", HTTP_POST, handleWifiReset);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Servidor web iniciado en puerto 80");
  Serial.print("Accede a http://");
  Serial.print(WiFi.localIP());
  Serial.println(" en tu navegador");
}

// ------------------------- Telemetría push -------------------------

// Aplica el estado deseado del output recibido del VPS (reutiliza la lógica de
// handleOutput). Acepta "on", "off" o vacío (sin comando -> no hace nada).
void applyDesiredOutput(const String &desired) {
  if (desired == "on") {
    digitalWrite(OUTPUT_PIN, HIGH);
  } else if (desired == "off") {
    digitalWrite(OUTPUT_PIN, LOW);
  }
}

// Extrae el valor de "output" del JSON de respuesta del VPS ({"output":"on"} etc.).
// Parser minimalista suficiente para esta respuesta pequeña (evita añadir ArduinoJson).
String parseDesiredOutput(const String &body) {
  int k = body.indexOf("\"output\"");
  if (k < 0) return "";
  int colon = body.indexOf(':', k);
  if (colon < 0) return "";
  int q1 = body.indexOf('"', colon + 1);
  if (q1 < 0) return "";  // valor null u otro no-string
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return body.substring(q1 + 1, q2);
}

// Envía el estado actual al VPS y aplica el comando recibido en la respuesta.
void pushTelemetry() {
  if (apMode || WiFi.status() != WL_CONNECTED) return;

  float temp = readTemperature();
  String outState = digitalRead(OUTPUT_PIN) == HIGH ? "on" : "off";

  String payload = "{";
  payload += "\"temp_c\":" + (isnan(temp) ? String("null") : String(temp, 2));
  payload += ",\"output\":\"" + outState + "\"";
  payload += ",\"rssi\":" + String(WiFi.RSSI());
  payload += ",\"ssid\":\"" + WiFi.SSID() + "\"";
  payload += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  payload += ",\"uptime_ms\":" + String(millis());
  payload += "}";

  WiFiClientSecure client;
#if VPS_TLS_INSECURE
  client.setInsecure();
#else
  client.setCACert(VPS_ROOT_CA);
#endif

  HTTPClient http;
  if (!http.begin(client, VPS_INGEST_URL)) {
    Serial.println("push: no se pudo iniciar la conexión al VPS");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Token", DEVICE_TOKEN);

  int code = http.POST(payload);
  if (code == 200) {
    String resp = http.getString();
    String desired = parseDesiredOutput(resp);
    if (desired.length() > 0) {
      applyDesiredOutput(desired);
    }
    Serial.print("push: OK (200), comando output=");
    Serial.println(desired.length() > 0 ? desired : String("(ninguno)"));
  } else {
    Serial.print("push: error HTTP ");
    Serial.println(code);
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\nIniciando ESP32...");

  // Salida digital controlable por API (arranca apagada)
  pinMode(OUTPUT_PIN, OUTPUT);
  digitalWrite(OUTPUT_PIN, LOW);

  // Recolectar headers personalizados (el WebServer no los capta por defecto)
  const char* headerKeys[] = {"X-API-Key", "x-api-key", "Authorization"};
  server.collectHeaders(headerKeys, 3);

  // Cargar credenciales (NVS o defaults) e intentar conectar
  String connSsid, connPass;
  loadCredentials(connSsid, connPass);

  if (connectToWifi(connSsid, connPass)) {
    startStaServer();
  } else {
    Serial.println("Sin conexión: arrancando portal para configurar una red.");
    startApPortal();
  }
}

void loop() {
  if (apMode) {
    dnsServer.processNextRequest();
  }
  server.handleClient();

  // Telemetría push periódica al VPS (no bloqueante)
  static unsigned long lastPush = 0;
  if (!apMode && millis() - lastPush >= PUSH_INTERVAL_MS) {
    lastPush = millis();
    pushTelemetry();
  }

  delay(1);
}
