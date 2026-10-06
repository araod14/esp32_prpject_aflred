'use strict';

const path = require('path');
const Database = require('better-sqlite3');

const DB_PATH = process.env.DB_PATH || path.join(__dirname, 'data.db');
const db = new Database(DB_PATH);

db.pragma('journal_mode = WAL');

db.exec(`
  CREATE TABLE IF NOT EXISTS readings (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    ts           INTEGER NOT NULL,          -- epoch ms de recepción
    temp_c       REAL,
    output_state TEXT,                      -- 'on' | 'off'
    rssi         INTEGER,
    ssid         TEXT,
    ip           TEXT,
    uptime_ms    INTEGER
  );

  CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings(ts);

  -- Estado deseado del output (una sola fila, id = 1)
  CREATE TABLE IF NOT EXISTS desired (
    id         INTEGER PRIMARY KEY CHECK (id = 1),
    output     TEXT,                        -- 'on' | 'off' | NULL (sin comando)
    updated_at INTEGER
  );

  INSERT OR IGNORE INTO desired (id, output, updated_at) VALUES (1, NULL, 0);
`);

const stmtInsert = db.prepare(`
  INSERT INTO readings (ts, temp_c, output_state, rssi, ssid, ip, uptime_ms)
  VALUES (@ts, @temp_c, @output_state, @rssi, @ssid, @ip, @uptime_ms)
`);

const stmtLatest = db.prepare(`SELECT * FROM readings ORDER BY ts DESC LIMIT 1`);

const stmtHistory = db.prepare(`
  SELECT ts, temp_c, output_state FROM readings
  WHERE ts >= @since ORDER BY ts ASC
`);

const stmtGetDesired = db.prepare(`SELECT output, updated_at FROM desired WHERE id = 1`);
const stmtSetDesired = db.prepare(`UPDATE desired SET output = @output, updated_at = @updated_at WHERE id = 1`);

// Limpieza opcional: borra lecturas más viejas que N días
const stmtPrune = db.prepare(`DELETE FROM readings WHERE ts < @cutoff`);

module.exports = {
  insertReading(r) {
    return stmtInsert.run({
      ts: r.ts,
      temp_c: r.temp_c ?? null,
      output_state: r.output_state ?? null,
      rssi: r.rssi ?? null,
      ssid: r.ssid ?? null,
      ip: r.ip ?? null,
      uptime_ms: r.uptime_ms ?? null,
    });
  },
  latestReading() {
    return stmtLatest.get() || null;
  },
  history(sinceMs) {
    return stmtHistory.all({ since: sinceMs });
  },
  getDesired() {
    return stmtGetDesired.get();
  },
  setDesired(output) {
    return stmtSetDesired.run({ output, updated_at: Date.now() });
  },
  prune(cutoffMs) {
    return stmtPrune.run({ cutoff: cutoffMs });
  },
};
