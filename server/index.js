'use strict';

require('dotenv').config();

const path = require('path');
const express = require('express');
const basicAuth = require('express-basic-auth');
const db = require('./db');

const PORT = parseInt(process.env.PORT || '8080', 10);
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || '';
const DASH_USER = process.env.DASH_USER || 'admin';
const DASH_PASS = process.env.DASH_PASS || '';
// Ventana (ms) sin telemetría tras la cual el dispositivo se considera offline.
const OFFLINE_AFTER_MS = parseInt(process.env.OFFLINE_AFTER_MS || '30000', 10);
// Retención del histórico en días (0 = sin límite).
const RETENTION_DAYS = parseInt(process.env.RETENTION_DAYS || '7', 10);

if (!DEVICE_TOKEN) {
  console.error('FATAL: falta DEVICE_TOKEN en el entorno (.env)');
  process.exit(1);
}
if (!DASH_PASS) {
  console.error('FATAL: falta DASH_PASS en el entorno (.env)');
  process.exit(1);
}

const app = express();
app.disable('x-powered-by');
app.use(express.json({ limit: '16kb' }));

// --- Comparación de tokens en tiempo constante ---
const crypto = require('crypto');
function safeEqual(a, b) {
  const ba = Buffer.from(String(a));
  const bb = Buffer.from(String(b));
  if (ba.length !== bb.length) return false;
  return crypto.timingSafeEqual(ba, bb);
}

// --- Auth del dispositivo (ESP32 -> /api/ingest) ---
function deviceAuth(req, res, next) {
  const token = req.get('X-Device-Token') || '';
  if (!token || !safeEqual(token, DEVICE_TOKEN)) {
    return res.status(401).json({ error: 'token de dispositivo inválido' });
  }
  next();
}

// --- Auth del dashboard (navegador) ---
const dashAuth = basicAuth({
  users: { [DASH_USER]: DASH_PASS },
  challenge: true,
  realm: 'ESP32 Monitor',
});

function normOutput(v) {
  if (v === 'on' || v === 'off') return v;
  return null;
}

// ------------------- Ingesta de telemetría (ESP32) -------------------
// El ESP32 envía su estado y recibe en la respuesta el estado deseado del output.
app.post('/api/ingest', deviceAuth, (req, res) => {
  const b = req.body || {};
  db.insertReading({
    ts: Date.now(),
    temp_c: typeof b.temp_c === 'number' ? b.temp_c : null,
    output_state: normOutput(b.output),
    rssi: Number.isFinite(b.rssi) ? b.rssi : null,
    ssid: typeof b.ssid === 'string' ? b.ssid.slice(0, 64) : null,
    ip: typeof b.ip === 'string' ? b.ip.slice(0, 45) : null,
    uptime_ms: Number.isFinite(b.uptime_ms) ? b.uptime_ms : null,
  });

  const desired = db.getDesired();
  res.json({ output: desired ? desired.output : null });
});

// ------------------- API del dashboard (protegida) -------------------
app.get('/api/state', dashAuth, (req, res) => {
  const latest = db.latestReading();
  const desired = db.getDesired();
  const online = latest ? Date.now() - latest.ts < OFFLINE_AFTER_MS : false;
  res.json({
    online,
    latest,
    desiredOutput: desired ? desired.output : null,
    serverTime: Date.now(),
  });
});

app.get('/api/history', dashAuth, (req, res) => {
  let minutes = parseInt(req.query.minutes || '60', 10);
  if (!Number.isFinite(minutes) || minutes <= 0) minutes = 60;
  minutes = Math.min(minutes, 60 * 24 * 7); // tope 7 días
  const since = Date.now() - minutes * 60 * 1000;
  res.json(db.history(since));
});

app.post('/api/command', dashAuth, (req, res) => {
  const desired = normOutput((req.body || {}).output);
  // output puede ser 'on', 'off' o null (limpiar comando)
  db.setDesired(desired);
  res.json({ ok: true, desiredOutput: desired });
});

// ------------------- Dashboard estático -------------------
app.use('/', dashAuth, express.static(path.join(__dirname, 'public')));

// Poda periódica del histórico
if (RETENTION_DAYS > 0) {
  setInterval(() => {
    try {
      db.prune(Date.now() - RETENTION_DAYS * 24 * 60 * 60 * 1000);
    } catch (e) {
      console.error('prune error:', e.message);
    }
  }, 60 * 60 * 1000).unref();
}

app.listen(PORT, '127.0.0.1', () => {
  console.log(`ESP32 monitor escuchando en http://127.0.0.1:${PORT}`);
});
