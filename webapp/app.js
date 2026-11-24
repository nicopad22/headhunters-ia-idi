'use strict';

// Headhunters — NFC card labelling via Web NFC (Chrome on Android).
//
// UID key format: the firmware identifies a tag by the FIRST 4 UID BYTES in the
// order they are sent over RF, packed big-endian (MFRC522 uid.uidByte[0..3]).
// Web NFC's serialNumber ("04:a2:3b:1c:...") uses the same byte order, so the
// key is simply its first 8 hex digits, uppercased. No byte swapping needed.

const STORAGE_KEY = 'hh-nfc-labels-v1';
const SCAN_COOLDOWN_MS = 1000;
const KIND_LABEL = { candidate: 'Postulante', occupation: 'Ocupación', mystery: 'Misterio' };
let GROUPS = [
  { kind: 'candidate', title: 'Postulantes' },
  { kind: 'occupation', title: 'Ocupaciones (Insight)' },
  { kind: 'mystery', title: 'Misterio' },
];

let cards = [];
let labels = {};          // code -> { uid: "04A23B1C", full: "04:A2:3B:1C:5D:6E:80", at: ISO }
let idx = 0;
let missingOnly = true;
let view = 'label';
let busy = false;         // true during the short "saved" animation before auto-advance
let dialogOpen = false;
let lastRead = { key: null, t: 0 };
let storageOk = true;
let audioCtx = null;
let wakeLock = null;
let nfcActive = false;

const $ = (id) => document.getElementById(id);

// ---------------------------------------------------------------- storage
function loadLabels() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) return JSON.parse(raw).labels || {};
  } catch (e) {
    storageOk = false;
  }
  return null;
}

function saveLabels() {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ labels, saved_at: new Date().toISOString() }));
  } catch (e) {
    if (storageOk) toast('No se pudo guardar en el teléfono. Exporta antes de cerrar.', true);
    storageOk = false;
  }
}

// ---------------------------------------------------------------- helpers
function parseUid(text) {
  const hex = String(text || '').replace(/[^0-9a-f]/gi, '').toUpperCase();
  if (hex.length < 8 || hex.length % 2 !== 0) return null;
  return { key: hex.slice(0, 8), full: hex.match(/../g).join(':') };
}

function assignedCount() {
  return cards.reduce((n, c) => n + (labels[c.code] ? 1 : 0), 0);
}

function ownerOf(key) {
  return cards.find((c) => labels[c.code] && labels[c.code].uid === key) || null;
}

function nextIndex(from, dir, onlyMissing) {
  const n = cards.length;
  for (let step = 1; step <= n; step++) {
    const i = (((from + dir * step) % n) + n) % n;
    if (!onlyMissing || !labels[cards[i].code]) return i;
  }
  return -1;
}

function firstMissing() {
  return cards.findIndex((c) => !labels[c.code]);
}

function beep(freq = 980, ms = 110) {
  try {
    if (!audioCtx) return;
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();
    osc.frequency.value = freq;
    gain.gain.setValueAtTime(0.15, audioCtx.currentTime);
    gain.gain.exponentialRampToValueAtTime(0.001, audioCtx.currentTime + ms / 1000);
    osc.connect(gain).connect(audioCtx.destination);
    osc.start();
    osc.stop(audioCtx.currentTime + ms / 1000);
  } catch (e) { /* audio is optional */ }
}

function buzz(pattern) {
  if (navigator.vibrate) navigator.vibrate(pattern);
}

let toastTimer = null;
function toast(msg, isErr = false) {
  const el = $('toast');
  el.textContent = msg;
  el.classList.toggle('err', isErr);
  el.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('show'), 2600);
}

function animateCard(cls) {
  const el = $('card');
  el.classList.remove('flash', 'shake');
  void el.offsetWidth;
  el.classList.add(cls);
}

function openDialog({ title, body, okText = 'Aceptar', input = false, danger = false }) {
  return new Promise((resolve) => {
    const dlg = $('dialog');
    $('dialog-title').textContent = title;
    $('dialog-body').textContent = body;
    const ok = $('dialog-ok');
    ok.textContent = okText;
    ok.classList.toggle('btn-danger', danger);
    ok.classList.toggle('btn-primary', !danger);
    const field = $('dialog-input');
    field.hidden = !input;
    field.value = '';
    dialogOpen = true;
    dlg.addEventListener('close', function onClose() {
      dlg.removeEventListener('close', onClose);
      dialogOpen = false;
      resolve({ ok: dlg.returnValue === 'ok', value: field.value });
    });
    dlg.returnValue = '';
    dlg.showModal();
    if (input) field.focus();
  });
}

// ---------------------------------------------------------------- render
function renderProgress() {
  const n = assignedCount();
  $('count').textContent = `${n}/${cards.length}`;
  $('progress-fill').style.width = `${(n / cards.length) * 100}%`;
}

function renderCard() {
  const c = cards[idx];
  if (!c) return;
  const label = labels[c.code];
  $('card-stripe').style.background = c.color;
  $('card-code').textContent = c.code;
  $('card-kind').textContent = KIND_LABEL[c.kind];
  $('card-pos').textContent = `${idx + 1} de ${cards.length}`;
  $('card-title').textContent = c.title;
  $('card-sub').textContent = c.subtitle;
  $('card-detail').textContent = c.detail;
  $('uid-box').dataset.assigned = label ? 'true' : 'false';
  $('uid-label').textContent = label ? 'Tarjeta asignada · UID' : 'Acerca esta tarjeta al teléfono';
  $('uid-value').textContent = label ? label.uid : '';
  $('btn-clear').disabled = !label;
  document.querySelectorAll('.mode-toggle button').forEach((b) => {
    b.setAttribute('aria-checked', String((b.dataset.mode === 'missing') === missingOnly));
  });
}

function renderList() {
  const root = $('list');
  root.replaceChildren();
  for (const g of GROUPS) {
    const items = cards.map((c, i) => ({ c, i })).filter(({ c }) => c.kind === g.kind);
    const done = items.filter(({ c }) => labels[c.code]).length;
    const head = document.createElement('h3');
    head.className = 'group-title';
    head.innerHTML = '<span></span><span></span>';
    head.children[0].textContent = g.title;
    head.children[1].textContent = `${done}/${items.length}`;
    const box = document.createElement('div');
    box.className = 'list-group';
    for (const { c, i } of items) {
      const row = document.createElement('button');
      row.type = 'button';
      row.className = 'row';
      row.style.setProperty('--c', c.color);
      const code = document.createElement('span');
      code.className = 'row-code';
      code.textContent = c.code;
      const name = document.createElement('span');
      name.className = 'row-name';
      name.textContent = c.title;
      const uid = document.createElement('span');
      const l = labels[c.code];
      uid.className = l ? 'row-uid' : 'row-uid empty';
      uid.textContent = l ? l.uid : 'falta';
      row.append(code, name, uid);
      row.addEventListener('click', () => {
        idx = i;
        if (labels[c.code]) missingOnly = false;
        showView('label');
      });
      box.append(row);
    }
    root.append(head, box);
  }
}

function renderExport() {
  const n = assignedCount();
  const missing = cards.filter((c) => !labels[c.code]).map((c) => c.code);
  $('export-summary').textContent = missing.length
    ? `${n} de ${cards.length} asignadas. Faltan: ${missing.slice(0, 12).join(', ')}${missing.length > 12 ? '…' : ''}`
    : `Las ${cards.length} tarjetas están asignadas. Listo para exportar.`;
}

function renderTest(parsed) {
  const box = $('test-result');
  box.replaceChildren();
  const owner = ownerOf(parsed.key);
  const stripe = document.createElement('div');
  stripe.className = 'card-stripe';
  stripe.style.background = owner ? owner.color : 'var(--danger)';
  const head = document.createElement('div');
  head.className = 'card-head';
  const code = document.createElement('span');
  code.className = 'code';
  code.textContent = owner ? owner.code : '—';
  const kind = document.createElement('span');
  kind.className = 'kind';
  kind.textContent = owner ? KIND_LABEL[owner.kind] : 'No registrada';
  head.append(code, kind);
  const title = document.createElement('h2');
  title.className = 'card-title';
  title.textContent = owner ? owner.title : 'Tarjeta sin asignar';
  const sub = document.createElement('p');
  sub.className = 'card-sub';
  sub.textContent = owner ? `${owner.subtitle} · ${owner.detail}` : 'El teléfono la lee bien. Asígnala en Etiquetar.';
  const uidBox = document.createElement('div');
  uidBox.className = 'uid-box';
  uidBox.dataset.assigned = owner ? 'true' : 'false';
  uidBox.innerHTML = '<span class="uid-label"></span><span class="uid-value"></span>';
  uidBox.children[0].textContent = `UID completo: ${parsed.full}`;
  uidBox.children[1].textContent = parsed.key;
  box.append(stripe, head, title, sub, uidBox);
}

function renderAll() {
  renderProgress();
  if (view === 'label') renderCard();
  if (view === 'list') renderList();
  if (view === 'export') renderExport();
}

function showView(name) {
  view = name;
  document.querySelectorAll('.view').forEach((v) => {
    v.dataset.active = String(v.id === `view-${name}`);
  });
  document.querySelectorAll('.tabs button').forEach((b) => {
    if (b.dataset.view === name) b.setAttribute('aria-current', 'page');
    else b.removeAttribute('aria-current');
  });
  window.scrollTo(0, 0);
  renderAll();
}

// ---------------------------------------------------------------- labelling flow
function setLabel(code, parsed) {
  labels[code] = { uid: parsed.key, full: parsed.full, at: new Date().toISOString() };
  saveLabels();
}

function advanceAfterSave(message) {
  busy = true;
  renderAll();
  animateCard('flash');
  beep(980, 110);
  buzz(60);
  toast(message);
  setTimeout(() => {
    busy = false;
    const next = missingOnly ? nextIndex(idx, +1, true) : idx + 1;
    if (next < 0 || next >= cards.length) {
      beep(1320, 220);
      buzz([60, 60, 120]);
      if (assignedCount() === cards.length) {
        toast(`¡Las ${cards.length} tarjetas listas! Ahora exporta.`);
        showView('export');
      } else {
        toast('Fin del recorrido.');
        renderAll();
      }
      return;
    }
    idx = next;
    renderAll();
  }, 650);
}

async function assignScan(parsed) {
  const cur = cards[idx];
  const owner = ownerOf(parsed.key);
  if (owner && owner.code === cur.code) {
    advanceAfterSave(`${cur.code} ya era esta tarjeta`);
    return;
  }
  if (owner) {
    beep(220, 220);
    buzz([80, 40, 80]);
    animateCard('shake');
    const res = await openDialog({
      title: 'Tarjeta ya asignada',
      body: `Esta tarjeta ya es ${owner.code} · ${owner.title}.\n¿Moverla a ${cur.code} · ${cur.title}?`,
      okText: `Mover a ${cur.code}`,
    });
    if (!res.ok) return;
    delete labels[owner.code];
  }
  const replaced = labels[cur.code] && labels[cur.code].uid !== parsed.key;
  setLabel(cur.code, parsed);
  advanceAfterSave(`${cur.code} ${replaced ? 'reemplazada' : 'guardada'} · ${parsed.key}`);
}

function onUid(serial) {
  const parsed = parseUid(serial);
  if (!parsed) {
    toast('La tarjeta no entregó un UID válido.', true);
    beep(220, 220);
    return;
  }
  // Treat repeated reads of the same tag within 1 s as one tap (tags can bounce).
  const now = Date.now();
  if (parsed.key === lastRead.key && now - lastRead.t < SCAN_COOLDOWN_MS) {
    lastRead.t = now;
    return;
  }
  lastRead = { key: parsed.key, t: now };
  if (dialogOpen || busy) return;

  if (view === 'test') {
    renderTest(parsed);
    const owner = ownerOf(parsed.key);
    beep(owner ? 980 : 440, 110);
    buzz(50);
  } else if (view === 'label') {
    assignScan(parsed);
  } else {
    const owner = ownerOf(parsed.key);
    toast(owner ? `${owner.code} · ${owner.title}` : `Sin asignar · ${parsed.key}`);
  }
}

// ---------------------------------------------------------------- Web NFC
function setBanner(state, text, showButton) {
  const b = $('nfc-banner');
  b.dataset.state = state;
  $('nfc-text').textContent = text;
  $('nfc-start').hidden = !showButton;
}

async function requestWakeLock() {
  try {
    if ('wakeLock' in navigator && document.visibilityState === 'visible') {
      wakeLock = await navigator.wakeLock.request('screen');
    }
  } catch (e) { /* optional */ }
}

async function startNfc() {
  try {
    if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  } catch (e) { /* optional */ }

  if (!('NDEFReader' in window)) {
    setBanner('unsupported', 'Este navegador no tiene Web NFC. Usa Chrome en Android (o “Ingresar a mano”).', false);
    return;
  }
  try {
    const reader = new NDEFReader();
    reader.addEventListener('reading', (e) => onUid(e.serialNumber));
    reader.addEventListener('readingerror', () => {
      toast('Se detectó una tarjeta pero Chrome no pudo leerla. Prueba otra vez o usa “Ingresar a mano”.', true);
      beep(220, 220);
    });
    await reader.scan();
    nfcActive = true;
    setBanner('on', 'Lector activo · acerca una tarjeta a la parte trasera', false);
    requestWakeLock();
  } catch (e) {
    let msg = `No se pudo activar NFC (${e.name}).`;
    if (e.name === 'NotAllowedError') msg = 'Permiso de NFC denegado. Actívalo en la configuración del sitio.';
    if (e.name === 'NotReadableError') msg = 'NFC apagado. Actívalo en Ajustes › Conexiones › NFC.';
    if (e.name === 'NotSupportedError') msg = 'Este teléfono no tiene NFC disponible.';
    setBanner('error', msg, true);
  }
}

async function maybeAutoStart() {
  if (!('NDEFReader' in window)) {
    setBanner('unsupported', 'Este navegador no tiene Web NFC. Usa Chrome en Android (o “Ingresar a mano”).', false);
    return;
  }
  try {
    const perm = await navigator.permissions.query({ name: 'nfc' });
    if (perm.state === 'granted') startNfc();
  } catch (e) { /* needs a tap on "Activar lector" */ }
}

// ---------------------------------------------------------------- export / import
function buildExport() {
  return {
    format: 'headhunters-nfc-uids',
    version: 1,
    exported_at: new Date().toISOString(),
    uid_key: 'First 4 UID bytes in RF order, hex (matches firmware uidToHash)',
    assigned: assignedCount(),
    total: cards.length,
    cards: cards.map((c) => ({
      code: c.code,
      kind: c.kind,
      index: c.index,
      name: c.title,
      uid: labels[c.code] ? labels[c.code].uid : '',
      uid_full: labels[c.code] ? labels[c.code].full : '',
    })),
  };
}

function exportFile() {
  const text = JSON.stringify(buildExport(), null, 2) + '\n';
  return new File([text], 'headhunters_nfc_uids.json', { type: 'application/json' });
}

function warnIfIncomplete() {
  const missing = cards.length - assignedCount();
  if (missing > 0) toast(`Ojo: faltan ${missing} tarjetas (se exportan vacías).`);
}

function download() {
  const file = exportFile();
  const url = URL.createObjectURL(file);
  const a = document.createElement('a');
  a.href = url;
  a.download = file.name;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 2000);
  warnIfIncomplete();
}

async function share() {
  const file = exportFile();
  try {
    await navigator.share({ files: [file], title: 'Headhunters NFC UIDs' });
  } catch (e) {
    if (e.name !== 'AbortError') toast('No se pudo compartir. Usa Descargar.', true);
  }
}

async function copyJson() {
  try {
    await navigator.clipboard.writeText(JSON.stringify(buildExport(), null, 2));
    toast('JSON copiado.');
    warnIfIncomplete();
  } catch (e) {
    toast('No se pudo copiar. Usa Descargar.', true);
  }
}

async function importFile(file) {
  let data;
  try {
    data = JSON.parse(await file.text());
  } catch (e) {
    toast('Ese archivo no es un JSON válido.', true);
    return;
  }
  if (!data || data.format !== 'headhunters-nfc-uids' || !Array.isArray(data.cards)) {
    toast('Ese JSON no es una exportación de esta app.', true);
    return;
  }
  const res = await openDialog({
    title: 'Importar respaldo',
    body: `Trae ${data.cards.filter((c) => c.uid).length} UIDs y reemplaza lo que hay ahora en este teléfono.`,
    okText: 'Importar',
  });
  if (!res.ok) return;
  const known = new Set(cards.map((c) => c.code));
  const next = {};
  for (const c of data.cards) {
    const parsed = parseUid(c.uid_full || c.uid);
    if (known.has(c.code) && parsed) {
      next[c.code] = { uid: parsed.key, full: parsed.full, at: data.exported_at };
    }
  }
  labels = next;
  saveLabels();
  idx = Math.max(0, firstMissing());
  toast(`Importadas ${Object.keys(next).length} tarjetas.`);
  renderAll();
}

// ---------------------------------------------------------------- wiring
function bindUi() {
  $('nfc-start').addEventListener('click', startNfc);

  document.querySelectorAll('.tabs button').forEach((b) => {
    b.addEventListener('click', () => showView(b.dataset.view));
  });

  document.querySelectorAll('.mode-toggle button').forEach((b) => {
    b.addEventListener('click', () => {
      missingOnly = b.dataset.mode === 'missing';
      if (missingOnly && labels[cards[idx].code]) {
        const m = nextIndex(idx, +1, true);
        if (m < 0) toast('No falta ninguna tarjeta.');
        else idx = m;
      }
      renderCard();
    });
  });

  $('btn-prev').addEventListener('click', () => {
    const i = missingOnly ? nextIndex(idx, -1, true) : (idx - 1 + cards.length) % cards.length;
    if (i < 0 || i === idx) toast('No hay otra tarjeta faltante.');
    else { idx = i; renderCard(); }
  });

  $('btn-next').addEventListener('click', () => {
    const i = missingOnly ? nextIndex(idx, +1, true) : (idx + 1) % cards.length;
    if (i < 0 || i === idx) toast('No hay otra tarjeta faltante.');
    else { idx = i; renderCard(); }
  });

  $('btn-clear').addEventListener('click', async () => {
    const c = cards[idx];
    if (!labels[c.code]) return;
    const res = await openDialog({
      title: `Borrar UID de ${c.code}`,
      body: `${c.title} quedará sin tarjeta asignada.`,
      okText: 'Borrar',
      danger: true,
    });
    if (!res.ok) return;
    delete labels[c.code];
    saveLabels();
    renderAll();
  });

  $('btn-manual').addEventListener('click', async () => {
    const c = cards[idx];
    const res = await openDialog({
      title: `UID para ${c.code}`,
      body: `${c.title}\nEscribe el UID en hex (ej. 04:A2:3B:1C o 04A23B1C), por ejemplo desde la app NFC Tools.`,
      okText: 'Guardar',
      input: true,
    });
    if (!res.ok) return;
    const parsed = parseUid(res.value);
    if (!parsed) {
      toast('UID inválido: necesita al menos 8 dígitos hex.', true);
      return;
    }
    assignScan(parsed);
  });

  $('btn-download').addEventListener('click', download);
  $('btn-copy').addEventListener('click', copyJson);
  try {
    const probe = new File(['{}'], 'x.json', { type: 'application/json' });
    if (navigator.canShare && navigator.canShare({ files: [probe] })) {
      $('btn-share').hidden = false;
      $('btn-share').addEventListener('click', share);
    }
  } catch (e) { /* no share */ }

  $('file-import').addEventListener('change', (e) => {
    const f = e.target.files && e.target.files[0];
    if (f) importFile(f);
    e.target.value = '';
  });

  $('btn-reset').addEventListener('click', async () => {
    const res = await openDialog({
      title: 'Borrar todo',
      body: `Se borran las ${assignedCount()} asignaciones guardadas en este teléfono. Exporta antes si las quieres conservar.`,
      okText: 'Borrar todo',
      danger: true,
    });
    if (!res.ok) return;
    labels = {};
    saveLabels();
    idx = 0;
    missingOnly = true;
    toast('Todo borrado.');
    renderAll();
  });

  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible' && nfcActive && !wakeLock) requestWakeLock();
    if (document.visibilityState === 'hidden') wakeLock = null;
  });
}

async function init() {
  bindUi();
  try {
    const res = await fetch('cards.json', { cache: 'no-cache' });
    const data = await res.json();
    cards = data.cards;
    if (Array.isArray(data.groups) && data.groups.length) GROUPS = data.groups;
  } catch (e) {
    setBanner('error', 'No se pudo cargar cards.json.', false);
    return;
  }
  $('list-title').textContent = `Las ${cards.length} tarjetas`;
  const saved = loadLabels();
  if (saved) {
    labels = saved;
  } else {
    // First visit: start from whatever UIDs game_data.json already has.
    labels = {};
    for (const c of cards) {
      const parsed = parseUid(c.nfc_uid);
      if (parsed) labels[c.code] = { uid: parsed.key, full: parsed.full, at: null };
    }
  }
  // Drop labels for codes that no longer exist.
  const known = new Set(cards.map((c) => c.code));
  for (const code of Object.keys(labels)) if (!known.has(code)) delete labels[code];

  const m = firstMissing();
  idx = m >= 0 ? m : 0;
  missingOnly = m >= 0;
  if (!storageOk) toast('Este navegador no guarda datos: exporta antes de cerrar.', true);
  renderAll();
  maybeAutoStart();
}

init();
