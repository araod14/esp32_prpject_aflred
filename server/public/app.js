'use strict';

const $ = (id) => document.getElementById(id);

// -------- Estado y helpers --------
function fmtAge(ms) {
  if (ms == null) return '--';
  const s = Math.floor(ms / 1000);
  if (s < 60) return `hace ${s}s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `hace ${m}m`;
  const h = Math.floor(m / 60);
  return `hace ${h}h`;
}

function fmtUptime(ms) {
  if (!Number.isFinite(ms)) return '--';
  const s = Math.floor(ms / 1000);
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m`;
}

async function api(path, opts) {
  const res = await fetch(path, opts);
  if (!res.ok) throw new Error(`${path} -> ${res.status}`);
  return res.json();
}

// -------- Refresco del estado --------
async function refreshState() {
  try {
    const s = await api('/api/state');
    const badge = $('status');
    if (s.online) {
      badge.textContent = 'ONLINE';
      badge.className = 'badge badge-on';
    } else {
      badge.textContent = 'OFFLINE';
      badge.className = 'badge badge-off';
    }

    const r = s.latest;
    if (r) {
      $('temp').textContent = r.temp_c != null ? r.temp_c.toFixed(2) : '--';
      $('output').textContent = r.output_state ? r.output_state.toUpperCase() : '--';
      $('ssid').textContent = r.ssid || '--';
      $('rssi').textContent = r.rssi != null ? r.rssi : '--';
      $('ip').textContent = r.ip || '--';
      $('lastseen').textContent = fmtAge(s.serverTime - r.ts);
      $('uptime').textContent = fmtUptime(r.uptime_ms);
    }

    const d = s.desiredOutput;
    $('cmd-hint').textContent = d
      ? `Forzando salida a "${d.toUpperCase()}" (el ESP32 lo aplica en el próximo push).`
      : 'Sin comando forzado (modo Auto).';
  } catch (e) {
    $('status').textContent = 'ERROR';
    $('status').className = 'badge badge-off';
  }
}

async function sendCommand(output) {
  await api('/api/command', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ output }),
  });
  refreshState();
}

$('btn-on').addEventListener('click', () => sendCommand('on'));
$('btn-off').addEventListener('click', () => sendCommand('off'));
$('btn-clear').addEventListener('click', () => sendCommand(null));

// -------- Chart de temperatura (canvas nativo) --------
const canvas = $('chart');
const ctx = canvas.getContext('2d');

function drawChart(points) {
  const dpr = window.devicePixelRatio || 1;
  const cssW = canvas.clientWidth || 900;
  const cssH = 320;
  canvas.width = cssW * dpr;
  canvas.height = cssH * dpr;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cssW, cssH);

  const pad = { l: 44, r: 12, t: 14, b: 26 };
  const W = cssW - pad.l - pad.r;
  const H = cssH - pad.t - pad.b;

  const styles = getComputedStyle(document.documentElement);
  const muted = styles.getPropertyValue('--muted').trim() || '#8a95ab';
  const accent = styles.getPropertyValue('--accent').trim() || '#4da3ff';
  const border = styles.getPropertyValue('--border').trim() || '#2b3446';

  const temps = points.map((p) => p.temp_c).filter((v) => v != null);
  if (points.length < 2 || temps.length === 0) {
    ctx.fillStyle = muted;
    ctx.font = '14px system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('Sin datos suficientes todavía…', cssW / 2, cssH / 2);
    return;
  }

  let min = Math.min(...temps);
  let max = Math.max(...temps);
  if (max - min < 1) { min -= 1; max += 1; }
  const t0 = points[0].ts;
  const t1 = points[points.length - 1].ts;
  const spanT = Math.max(1, t1 - t0);

  const x = (ts) => pad.l + ((ts - t0) / spanT) * W;
  const y = (v) => pad.t + H - ((v - min) / (max - min)) * H;

  // Grid + etiquetas Y
  ctx.strokeStyle = border;
  ctx.fillStyle = muted;
  ctx.font = '11px system-ui, sans-serif';
  ctx.textAlign = 'right';
  ctx.lineWidth = 1;
  const ticks = 4;
  for (let i = 0; i <= ticks; i++) {
    const v = min + ((max - min) * i) / ticks;
    const yy = y(v);
    ctx.beginPath();
    ctx.moveTo(pad.l, yy);
    ctx.lineTo(cssW - pad.r, yy);
    ctx.stroke();
    ctx.fillText(v.toFixed(1), pad.l - 6, yy + 3);
  }

  // Etiquetas X (inicio / fin)
  ctx.textAlign = 'left';
  ctx.fillText(new Date(t0).toLocaleTimeString(), pad.l, cssH - 8);
  ctx.textAlign = 'right';
  ctx.fillText(new Date(t1).toLocaleTimeString(), cssW - pad.r, cssH - 8);

  // Línea
  ctx.strokeStyle = accent;
  ctx.lineWidth = 2;
  ctx.beginPath();
  let started = false;
  for (const p of points) {
    if (p.temp_c == null) continue;
    const px = x(p.ts);
    const py = y(p.temp_c);
    if (!started) { ctx.moveTo(px, py); started = true; }
    else ctx.lineTo(px, py);
  }
  ctx.stroke();
}

async function refreshChart() {
  try {
    const minutes = $('range').value;
    const points = await api(`/api/history?minutes=${minutes}`);
    drawChart(points);
  } catch (e) {
    /* silencioso */
  }
}

$('range').addEventListener('change', refreshChart);
window.addEventListener('resize', refreshChart);

// -------- Loop de refresco --------
refreshState();
refreshChart();
setInterval(refreshState, 5000);
setInterval(refreshChart, 15000);
