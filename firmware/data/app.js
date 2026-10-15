/* Photo Frame web UI. Vanilla JS, no build step, no requests outside the device's own API. */
(() => {
  'use strict';

  // --- Tunables -------------------------------------------------------------
  const MAX_EDGE = 1920;              // long edge of uploaded photos, never upscaled
  const JPEG_QUALITY = 0.85;
  const THUMB_EDGE = 320;
  const THUMB_CONCURRENCY = 2;        // the device serves roughly 500 KB/s
  const EST_MAX_BYTES = 1024 * 1024;  // per-photo size guess for the up-front space check
  const SPACE_MARGIN = 64 * 1024;     // FAT cluster slack and directory growth
  const POLL_MS = 5000;
  const IMG_RE = /\.(jpe?g|png|gif|bmp|webp|heic|heif)$/i;

  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => [...r.querySelectorAll(s)];
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const store = {
    get(k) { try { return localStorage.getItem('pf.' + k); } catch { return null; } },
    set(k, v) { try { localStorage.setItem('pf.' + k, v); } catch {} },
    del(k) { try { localStorage.removeItem('pf.' + k); } catch {} }
  };

  // --- Mock device (used when /api/status is unreachable, or with ?mock) -------
  const FORCE_MOCK = /[?&]mock(=1|=true)?(&|$)/.test(location.search);
  const MOCK_STATUS = {
    name: 'Photo Frame', version: '0.1.0', hostname: 'photoframe', ip: '192.168.1.50', ssid: 'HomeWifi', rssi: -52, mode: 'sta',
    card: { present: true, totalBytes: 31896633344, usedBytes: 8421376000, freeBytes: 23475257344 },
    usb: { hostPresent: true, mediaPresent: true, batchPending: false },
    ota: { enabled: true, version: '0.1.0', sdAvailable: false }, auth: { required: false, user: 'admin' }, uptimeMs: 12345678
  };
  const MOCK_FS = { '/': [], '/Holidays': [] };
  const MOCK_BLOBS = new Map();       // path -> uploaded blob
  (function seedMock() {
    for (let i = 1; i <= 17; i++) MOCK_FS['/'].push({ name: '20260101_12' + String(i).padStart(2, '0') + '00_01.jpg', dir: false, size: 300000 + i * 13000 });
    MOCK_FS['/'].push({ name: 'Holidays', dir: true, size: 0 }, { name: 'notes.txt', dir: false, size: 812 });
    for (let i = 1; i <= 4; i++) MOCK_FS['/Holidays'].push({ name: '20250720_09' + String(i).padStart(2, '0') + '00_01.jpg', dir: false, size: 410000 });
  })();
  async function mockBlob(path) {
    if (MOCK_BLOBS.has(path)) return MOCK_BLOBS.get(path);
    let h = 0; for (const ch of path) h = (h * 31 + ch.charCodeAt(0)) >>> 0;
    const portrait = h % 3 === 0, w = portrait ? 600 : 900, ht = portrait ? 900 : 600;
    const c = document.createElement('canvas'); c.width = w; c.height = ht;
    const x = c.getContext('2d'), g = x.createLinearGradient(0, 0, w, ht);
    g.addColorStop(0, 'hsl(' + (h % 360) + ',60%,55%)'); g.addColorStop(1, 'hsl(' + ((h >> 3) % 360) + ',60%,30%)');
    x.fillStyle = g; x.fillRect(0, 0, w, ht);
    x.fillStyle = 'rgba(255,255,255,.85)'; x.font = '40px sans-serif'; x.fillText(path.split('/').pop().slice(0, 15), 30, 70);
    return new Promise(res => c.toBlob(res, 'image/jpeg', 0.8));
  }
  function mockSplit(p) { const i = p.lastIndexOf('/'); return { dir: i <= 0 ? '/' : p.slice(0, i), name: p.slice(i + 1) }; }
  async function mockPost(url, data) {
    await sleep(120);
    if (url === '/api/batch/begin') { MOCK_STATUS.usb.batchPending = true; MOCK_STATUS.usb.mediaPresent = false; }
    else if (url === '/api/batch/end' || url === '/api/eject' || url === '/api/restart') {
      MOCK_STATUS.usb.mediaPresent = false;
      setTimeout(() => { MOCK_STATUS.usb.batchPending = false; MOCK_STATUS.usb.mediaPresent = true; }, 1500);
    } else if (url === '/api/delete') {
      const { dir, name } = mockSplit(data.path);
      MOCK_FS[dir] = (MOCK_FS[dir] || []).filter(e => e.name !== name); delete MOCK_FS[data.path]; MOCK_BLOBS.delete(data.path);
    } else if (url === '/api/mkdir') {
      const { dir, name } = mockSplit(data.path);
      if ((MOCK_FS[dir] || []).some(e => e.name === name)) return { ok: false, error: 'Already exists' };
      (MOCK_FS[dir] = MOCK_FS[dir] || []).push({ name, dir: true, size: 0 }); MOCK_FS[data.path] = [];
    }
    return { ok: true };
  }
  async function mockUpload(it, blob) {
    const total = Math.max(600, blob.size / 800); // ms, roughly 800 KB/s
    for (let t = 0; t < total; t += 100) { await sleep(100); setProgress(it, Math.min(1, t / total)); }
    const path = it.dir === '/' ? '/' + it.name : it.dir + '/' + it.name;
    (MOCK_FS[it.dir] = MOCK_FS[it.dir] || []).push({ name: it.name, dir: false, size: blob.size });
    MOCK_BLOBS.set(path, blob);
    MOCK_STATUS.card.usedBytes += blob.size; MOCK_STATUS.card.freeBytes -= blob.size;
    return { ok: true };
  }

  // --- State ------------------------------------------------------------------
  let useMock = false;
  let status = null;
  let curPath = '/';
  let entries = [];
  let selMode = false;
  const sel = new Set();
  let gen = 0;                        // bumped on every folder load; stale thumbnail work is dropped
  let holds = 0;                      // >0 while an upload, delete or full-size view needs the device to itself
  let pollTimer = null;
  let authUser = store.get('authUser') || '';
  let authPass = store.get('authPass') || '';

  function hold(d) { holds += d; if (holds <= 0) { holds = 0; pumpThumbs(); } }

  // --- Helpers ----------------------------------------------------------------
  function fmtBytes(n) {
    if (n == null || isNaN(n)) return '-';
    if (n === 0) return '0 B';
    const u = ['B', 'KB', 'MB', 'GB', 'TB']; let i = 0, v = n;
    while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
    return (v >= 10 || i === 0 ? Math.round(v) : Math.round(v * 10) / 10) + ' ' + u[i];
  }
  function fmtUptime(ms) {
    const s = Math.floor(ms / 1000), d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
    return d ? d + 'd ' + h + 'h' : h ? h + 'h ' + m + 'm' : m ? m + 'm ' + (s % 60) + 's' : s + 's';
  }
  function rssiLabel(r) {
    if (r == null) return '-';
    return (r >= -50 ? 'Excellent' : r >= -60 ? 'Good' : r >= -70 ? 'Fair' : 'Weak') + ' (' + r + ' dBm)';
  }
  const pad = (n, w = 2) => String(n).padStart(w, '0');
  const plural = (n, w) => n + ' ' + w + (n === 1 ? '' : 's');
  function normPath(p) {
    if (!p.startsWith('/')) p = '/' + p;
    p = p.replace(/\/+/g, '/'); if (p.length > 1 && p.endsWith('/')) p = p.slice(0, -1);
    return p || '/';
  }
  const joinPath = (base, name) => normPath(base === '/' ? '/' + name : base + '/' + name);
  const isImageName = n => IMG_RE.test(n);

  function toast(msg, kind) {
    const w = $('#toastWrap'), t = document.createElement('div');
    t.className = 'toast' + (kind === 'error' ? ' toast--err' : '');
    t.textContent = msg; w.appendChild(t);
    setTimeout(() => { t.style.opacity = '0'; }, 2800);
    setTimeout(() => t.remove(), 3200);
  }

  // --- Auth ---------------------------------------------------------------------
  function authHeader() { return authUser && authPass ? 'Basic ' + btoa(unescape(encodeURIComponent(authUser + ':' + authPass))) : null; }
  // A typed password is kept in memory until the device accepts it, and only then saved; a password
  // the device rejects is dropped from storage so a stale one is not resent on every visit.
  let authUnverified = false, authCancelled = false;
  function setAuth(u, p) { authUser = u; authPass = p; authUnverified = true; }
  // Both take the Authorization header the request was sent with, so a reply to an older credential
  // (a thumbnail in flight when the password changed) neither saves nor wipes the current one.
  function authAccepted(sent) {
    if (!sent || sent !== authHeader()) return;
    if (authUnverified) { authUnverified = false; store.set('authUser', authUser); store.set('authPass', authPass); }
    if (thumbFailed.size) { thumbFailed.clear(); reviveThumbs(); }   // 401s from before the login
  }
  function authRejected(sent) {
    if (sent !== authHeader()) return;
    // Leave storage alone if another tab has since saved a different password.
    if (store.get('authPass') === authPass) store.set('authPass', '');
    authPass = ''; authUnverified = false;
  }
  // These answer without a login, so a 200 from them proves nothing about the password.
  const OPEN_ENDPOINTS = ['/api/status', '/api/auth/status'];
  function withAuthHeaders(h = {}) { const a = authHeader(); if (a) h['Authorization'] = a; return h; }
  // The username comes from the device's status (admin unless changed on the Login tab); only the
  // password is asked. After Cancel the page stops asking until the user starts something new, so a
  // 20-photo upload with a wrong password is one prompt, not twenty.
  async function promptAuth() {
    if (authCancelled) return false;
    // openModal() replaces an open dialog with false; a background 401 must not cancel the user's
    // Delete or Restart confirmation, so wait for it to close first.
    while (!$('#modal').classList.contains('hidden')) await sleep(200);
    if (authCancelled) return false;
    const u = authUser || 'admin';
    const p = await openModal({ title: 'Password', msg: 'Password for ' + u, showInput: true, password: true, placeholder: 'password', okText: 'Log in' });
    if (p === false) { authCancelled = true; return false; }
    setAuth(u, String(p));
    return true;
  }
  let authPending = null;
  function handleAuthFail(sent) {
    // A 401 for an older credential (sent before a login or a password change) says nothing about the
    // current one: retry with it instead of asking again.
    if (sent !== authHeader() && authHeader()) return Promise.resolve(true);
    authRejected(sent);
    if (!authPending) authPending = promptAuth().finally(() => { authPending = null; });
    return authPending;
  }
  function userAction() { authCancelled = false; }
  // Any click is a new action: after Cancel, the next thing the user does may prompt again.
  document.addEventListener('click', userAction, true);

  // --- API (mock aware) -----------------------------------------------------------
  async function request(url, opts = {}) {
    for (let tries = 0; ; tries++) {
      const sent = authHeader();
      const r = await fetch(url, { cache: 'no-store', ...opts, headers: withAuthHeaders({ ...(opts.headers || {}) }) });
      if (r.status === 401 && tries < 1 && await handleAuthFail(sent)) continue;
      if (r.status === 401) authRejected(sent);
      else if (r.ok && !OPEN_ENDPOINTS.some(e => url.startsWith(e))) authAccepted(sent);
      return r;
    }
  }
  async function apiStatus() {
    if (useMock) { await sleep(60); return JSON.parse(JSON.stringify(MOCK_STATUS)); }
    const r = await request('/api/status');
    if (!r.ok) throw new Error('status ' + r.status);
    return r.json();
  }
  async function apiList(path) {
    if (useMock) { await sleep(80); return { path, entries: JSON.parse(JSON.stringify(MOCK_FS[path] || [])) }; }
    const r = await request('/api/list?path=' + encodeURIComponent(path));
    if (!r.ok) { const j = await r.json().catch(() => ({})); throw new Error(j.error || 'HTTP ' + r.status); }
    return r.json();
  }
  async function apiPost(url, data = {}) {
    if (useMock) return mockPost(url, data);
    try {
      const r = await request(url, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams(data).toString() });
      return await r.json().catch(() => ({ ok: false, error: 'Bad response (HTTP ' + r.status + ')' }));
    } catch (e) { return { ok: false, error: 'Network error' }; }
  }
  async function apiWifiScan() {
    if (useMock) { await sleep(400); return { networks: [{ ssid: 'HomeWifi', rssi: -46, secure: true }, { ssid: 'Guest', rssi: -62, secure: false }] }; }
    const r = await request('/api/wifi/scan'); if (!r.ok) throw new Error('HTTP ' + r.status); return r.json();
  }
  async function fetchBlob(path) {
    if (useMock) { await sleep(150); return mockBlob(path); }
    // One 45 s budget per attempt, so time spent at a password prompt between attempts does not count.
    for (let attempt = 0; ; attempt++) {
      const ac = new AbortController(), t = setTimeout(() => ac.abort(), 45000);
      try {
        const sent = authHeader();
        const r = await fetch('/api/download?path=' + encodeURIComponent(path), { cache: 'no-store', signal: ac.signal, headers: withAuthHeaders() });
        if (r.status === 401 && attempt === 0 && await handleAuthFail(sent)) continue;
        if (r.status === 401) { authRejected(sent); throw new Error('HTTP 401'); }
        if (!r.ok) throw new Error('HTTP ' + r.status);
        authAccepted(sent);
        return await r.blob();
      } finally { clearTimeout(t); }
    }
  }

  // --- Image decode and re-encode ----------------------------------------------------
  // An <img> honours EXIF orientation when drawn to a canvas in current Safari, Chrome and Firefox;
  // createImageBitmap is the fallback when the <img> cannot decode the file.
  async function decode(blob) {
    const url = URL.createObjectURL(blob);
    try {
      const img = new Image(); img.src = url; await img.decode();
      if (!img.naturalWidth) throw new Error('empty image');
      return { src: img, w: img.naturalWidth, h: img.naturalHeight, done: () => URL.revokeObjectURL(url) };
    } catch (e) {
      URL.revokeObjectURL(url);
      if (!window.createImageBitmap) throw e;
      let bm; try { bm = await createImageBitmap(blob, { imageOrientation: 'from-image' }); } catch { bm = await createImageBitmap(blob); }
      return { src: bm, w: bm.width, h: bm.height, done: () => { try { bm.close(); } catch {} } };
    }
  }
  async function reencode(blob, maxEdge, quality) {
    const d = await decode(blob);
    try {
      const sc = Math.min(1, maxEdge / Math.max(d.w, d.h));
      const w = Math.max(1, Math.round(d.w * sc)), h = Math.max(1, Math.round(d.h * sc));
      const c = document.createElement('canvas'); c.width = w; c.height = h;
      const x = c.getContext('2d');
      x.fillStyle = '#fff'; x.fillRect(0, 0, w, h);   // transparent PNGs would otherwise turn black
      x.imageSmoothingQuality = 'high';
      x.drawImage(d.src, 0, 0, w, h);
      const out = await new Promise(res => c.toBlob(res, 'image/jpeg', quality));
      c.width = c.height = 0;                           // release the bitmap memory on iOS
      if (!out) throw new Error('encode failed');
      return out;
    } finally { d.done(); }
  }

  // --- Thumbnails: lazy, 2 at a time, cached in memory and IndexedDB, never written to the card ---
  const memCache = new Map();
  let dbp = null;
  function db() {
    if (!dbp) dbp = new Promise((res, rej) => {
      try {
        const r = indexedDB.open('pf-thumbs', 1);
        r.onupgradeneeded = () => r.result.createObjectStore('t');
        r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
      } catch (e) { rej(e); }
    });
    return dbp;
  }
  async function cacheGet(key) {
    if (memCache.has(key)) return memCache.get(key);
    if (useMock) return null;
    try {
      const d = await db();
      const v = await new Promise(res => { const q = d.transaction('t').objectStore('t').get(key); q.onsuccess = () => res(q.result || null); q.onerror = () => res(null); });
      if (v) memCache.set(key, v);
      return v;
    } catch { return null; }
  }
  async function cachePut(key, blob) {
    memCache.set(key, blob);
    if (useMock) return;
    try { const d = await db(); d.transaction('t', 'readwrite').objectStore('t').put(blob, key); } catch {}
  }

  const thumbFailed = new Set();
  const thumbQ = [];
  let thumbActive = 0;
  const io = 'IntersectionObserver' in window ? new IntersectionObserver(onIntersect, { rootMargin: '300px' }) : null;
  function onIntersect(list) {
    for (const e of list) {
      const t = e.target;
      if (e.isIntersecting && t._st === 0 && !thumbFailed.has(t.dataset.path + '|' + t.dataset.size)) { t._st = 1; thumbQ.push(t); }
      else if (!e.isIntersecting && t._st === 1) { t._st = 0; const i = thumbQ.indexOf(t); if (i >= 0) thumbQ.splice(i, 1); }
    }
    pumpThumbs();
  }
  // Tiles whose thumbnail failed are left at _st 3 and unobserved; start them over.
  function reviveThumbs() {
    $$('.tile').forEach(t => { if (t._st === 3 && !t.classList.contains('has-img')) { t._st = 0; if (io) io.observe(t); } });
    pumpThumbs();
  }
  function pumpThumbs() {
    while (holds === 0 && thumbActive < THUMB_CONCURRENCY && thumbQ.length) loadThumb(thumbQ.shift());
  }
  async function loadThumb(t) {
    t._st = 2; thumbActive++;
    const g = gen, path = t.dataset.path, key = path + '|' + t.dataset.size;
    try {
      let blob = await cacheGet(key);
      if (!blob) { blob = await reencode(await fetchBlob(path), THUMB_EDGE, 0.7); cachePut(key, blob); }
      if (g === gen) {
        const img = t.querySelector('img'), u = URL.createObjectURL(blob);
        img.onload = () => { URL.revokeObjectURL(u); t.classList.add('has-img'); };
        img.src = u;
      }
    } catch { thumbFailed.add(key); /* leave the placeholder icon */ }
    finally { t._st = 3; thumbActive--; if (io) io.unobserve(t); pumpThumbs(); }
  }

  // --- Navigation, theme ----------------------------------------------------------------
  function showView(id) {
    $$('.view').forEach(v => v.classList.toggle('is-active', v.id === 'view-' + id));
    $$('.tab').forEach(b => b.classList.toggle('is-active', b.dataset.view === id));
    window.scrollTo(0, 0);
  }
  $$('.tab').forEach(b => b.addEventListener('click', () => showView(b.dataset.view)));
  $$('[data-jump]').forEach(b => b.addEventListener('click', () => showView(b.dataset.jump)));
  function showTab(name) {
    $$('.subtab').forEach(b => b.classList.toggle('is-active', b.dataset.tab === name));
    $$('.tabpanel').forEach(p => p.classList.toggle('is-active', p.id === 'tab-' + name));
    store.set('tab', name);
  }
  $$('.subtab').forEach(b => b.addEventListener('click', () => showTab(b.dataset.tab)));
  showTab(store.get('tab') || 'frame');

  function applyTheme(v) { if (v === 'dark' || v === 'light') document.documentElement.setAttribute('data-theme', v); else document.documentElement.removeAttribute('data-theme'); }
  applyTheme(store.get('theme'));
  $('#themeToggle').addEventListener('click', () => {
    const cur = document.documentElement.getAttribute('data-theme');
    const next = !cur ? (matchMedia('(prefers-color-scheme: dark)').matches ? 'light' : 'dark') : cur === 'dark' ? 'light' : 'dark';
    store.set('theme', next); applyTheme(next);
  });

  // --- Modal -----------------------------------------------------------------------------
  let modalResolve = null;
  function openModal({ title, msg, placeholder, inputValue, okText, showInput, password }) {
    if (modalResolve) closeModal(false);
    $('#modalTitle').textContent = title || ''; $('#modalMsg').textContent = msg || '';
    const wrap = $('#modalInputWrap'), inp = $('#modalInput');
    wrap.classList.toggle('hidden', !showInput);
    if (showInput) { inp.type = password ? 'password' : 'text'; inp.value = inputValue || ''; inp.placeholder = placeholder || ''; setTimeout(() => inp.focus(), 30); }
    $('#modalOk').textContent = okText || 'OK';
    $('#modal').classList.remove('hidden');
    return new Promise(res => { modalResolve = res; });
  }
  function closeModal(v) { $('#modal').classList.add('hidden'); if (modalResolve) { const r = modalResolve; modalResolve = null; r(v); } }
  $('#modalCancel').addEventListener('click', () => closeModal(false));
  $('#modalBg').addEventListener('click', () => closeModal(false));
  $('#modalOk').addEventListener('click', () => closeModal($('#modalInputWrap').classList.contains('hidden') ? true : $('#modalInput').value));
  $('#modalInput').addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); closeModal(e.target.value); } });
  document.addEventListener('keydown', e => {
    if (e.key !== 'Escape') return;
    if (!$('#modal').classList.contains('hidden')) closeModal(false); else if (!$('#viewer').classList.contains('hidden')) closeViewer();
  });

  // --- Status line -------------------------------------------------------------------------
  function setChip(id, cls, text) { const c = $(id); c.className = 'chip ' + cls; c.lastElementChild.textContent = text; }
  function renderStatus(s) {
    status = s;
    const card = s.card || {}, usb = s.usb || {};
    // The device's username wins over a stored one, which is stale after a change on the Login tab.
    if (s.auth && s.auth.user && s.auth.user !== authUser) { authUser = s.auth.user; store.set('authUser', authUser); }
    $('#versionBadge').textContent = s.version || '';
    $('#aboutVersion').textContent = s.version || '-';
    if (useMock) setChip('#chipWifi', 'warn', 'Preview (no device)');
    else if (s.mode === 'ap') setChip('#chipWifi', 'warn', 'Setup network, not on home Wi-Fi');
    else setChip('#chipWifi', 'ok', s.ssid ? s.ssid + ' ' + (s.rssi != null ? s.rssi + ' dBm' : '') : 'Wi-Fi');
    if (!card.present) setChip('#chipCard', 'err', 'No SD card');
    else setChip('#chipCard', 'ok', fmtBytes(card.freeBytes) + ' free of ' + fmtBytes(card.totalBytes));
    if (usb.batchPending) setChip('#chipFrame', 'pend', 'Frame refresh pending');
    else if (!usb.hostPresent) setChip('#chipFrame', 'warn', 'Frame not connected');
    else setChip('#chipFrame', 'ok', 'Frame up to date');

    $('#usbMsg').textContent = !usb.hostPresent && !usb.batchPending ? 'The frame is not connected, or has not mounted the drive.'
      : usb.batchPending ? 'A batch is open. The frame picks up the new photos when it ends.'
      : usb.mediaPresent ? 'Frame connected and able to read the card.'
      : 'Frame connected, but the card is not presented (no card, or a refresh is under way).';
    $('#kvHostname').textContent = s.hostname || '-';
    $('#kvIp').textContent = s.ip || '-';
    $('#kvSsid').textContent = s.ssid || '-';
    $('#kvRssi').textContent = rssiLabel(s.rssi);
    $('#kvMode').textContent = s.mode === 'ap' ? 'Setup access point' : 'Home Wi-Fi';
    $('#kvCard').textContent = card.present ? fmtBytes(card.usedBytes) + ' used, ' + fmtBytes(card.freeBytes) + ' free of ' + fmtBytes(card.totalBytes) : 'No card';
    $('#kvUptime').textContent = fmtUptime(s.uptimeMs || 0);
    if (s.hostname) $('#discInfo').textContent = s.hostname + '.local';
    $('#otaCurrent').textContent = s.ota ? s.ota.version : (s.version || '-');
    $('#otaEnabledBadge').textContent = s.ota && s.ota.enabled ? '(OTA on)' : '(OTA off)';
    const avail = !!(s.ota && s.ota.sdAvailable);
    $('#otaSDBox').classList.toggle('hidden', !avail);
    if (avail) $('#otaSDVer').textContent = s.ota.sdVersion || '';
  }
  async function refreshStatus() {
    try { renderStatus(await apiStatus()); }
    catch { setChip('#chipWifi', 'err', 'Offline'); }
  }
  function startPolling() {
    clearInterval(pollTimer);
    pollTimer = setInterval(() => { if (holds === 0 && !document.hidden) refreshStatus(); }, POLL_MS);
  }
  document.addEventListener('visibilitychange', () => { if (!document.hidden && holds === 0) refreshStatus(); });

  // --- Gallery ------------------------------------------------------------------------------
  async function loadPath(p) {
    p = normPath(p);
    if (p !== curPath) { sel.clear(); }
    curPath = p; gen++;
    thumbFailed.clear();   // a reboot or a login since then may have fixed them
    $('#destLabel').textContent = p;
    try {
      entries = (await apiList(p)).entries || [];
      $('#loadErr').classList.add('hidden');
    } catch (e) {
      entries = [];
      $('#loadErrText').textContent = 'Could not read ' + p + ': ' + e.message + '.';
      $('#loadErr').classList.remove('hidden');
    }
    renderCrumbs(); renderGrid(); updateSelBar();
  }
  function renderCrumbs() {
    const ol = $('#crumbs'); ol.textContent = '';
    const parts = curPath.split('/').filter(Boolean), crumbs = [{ n: 'Photos', p: '/' }];
    let acc = ''; parts.forEach(s => { acc += '/' + s; crumbs.push({ n: s, p: acc }); });
    crumbs.forEach((c, i) => {
      const li = document.createElement('li');
      if (i === crumbs.length - 1) { const s = document.createElement('span'); s.setAttribute('aria-current', 'page'); s.textContent = c.n; li.appendChild(s); }
      else { const b = document.createElement('button'); b.type = 'button'; b.textContent = c.n; b.addEventListener('click', () => loadPath(c.p)); li.appendChild(b); }
      ol.appendChild(li);
    });
  }
  function sortedEntries() {
    // Folders first, then newest first: the upload names sort by time.
    return [...entries].sort((a, b) => a.dir !== b.dir ? (a.dir ? -1 : 1) : (a.dir ? a.name.localeCompare(b.name) : b.name.localeCompare(a.name)));
  }
  function svgUse(id, cls) {
    const s = document.createElementNS('http://www.w3.org/2000/svg', 'svg'), u = document.createElementNS('http://www.w3.org/2000/svg', 'use');
    s.setAttribute('class', cls || 'ico'); u.setAttribute('href', '#' + id); s.appendChild(u); return s;
  }
  function renderGrid() {
    const grid = $('#grid'); grid.textContent = '';
    thumbQ.length = 0;
    const list = sortedEntries();
    $('#empty').classList.toggle('hidden', list.length > 0 || !$('#loadErr').classList.contains('hidden'));
    const frag = document.createDocumentFragment();
    for (const ent of list) {
      const t = document.createElement('button');
      const photo = !ent.dir && isImageName(ent.name);
      t.type = 'button'; t.className = 'tile' + (ent.dir ? ' tile--dir' : '') + (sel.has(ent.name) ? ' is-sel' : '');
      t.dataset.name = ent.name; t.dataset.path = joinPath(curPath, ent.name); t.dataset.size = ent.size; t.dataset.dir = ent.dir ? '1' : '';
      t.setAttribute('aria-label', (ent.dir ? 'Folder ' : '') + ent.name);
      if (photo) { const img = document.createElement('img'); img.alt = ''; img.decoding = 'async'; t.appendChild(img); }
      t.appendChild(svgUse(ent.dir ? 'i-folder' : 'i-file'));
      const nm = document.createElement('span'); nm.className = 'tile__name' + (photo ? ' is-photo' : ''); nm.textContent = ent.name; t.appendChild(nm);
      const tick = document.createElement('span'); tick.className = 'tick'; tick.appendChild(svgUse('i-check')); t.appendChild(tick);
      t._st = photo ? 0 : 3;
      t.addEventListener('click', () => onTile(t, ent));
      frag.appendChild(t);
      if (photo) { if (io) io.observe(t); else { t._st = 1; thumbQ.push(t); } }
    }
    grid.appendChild(frag);
    grid.classList.toggle('selmode', selMode);
    $('#view-photos').classList.toggle('selmode', selMode);
    pumpThumbs();
  }
  function onTile(t, ent) {
    if (selMode) { if (sel.has(ent.name)) sel.delete(ent.name); else sel.add(ent.name); t.classList.toggle('is-sel', sel.has(ent.name)); updateSelBar(); return; }
    if (ent.dir) loadPath(joinPath(curPath, ent.name));
    else if (isImageName(ent.name)) openViewer(ent.name);
    else downloadFile(ent.name);
  }
  function setSelMode(on) {
    selMode = on; if (!on) sel.clear();
    $('#selbar').classList.toggle('hidden', !on);
    $('#selectBtn').classList.toggle('hidden', on);
    $('#view-photos').classList.toggle('selmode', on);
    $$('#grid .tile').forEach(t => t.classList.toggle('is-sel', sel.has(t.dataset.name)));
    updateSelBar();
  }
  function updateSelBar() {
    $('#selCount').textContent = sel.size + ' selected';
    $('#delBtn').disabled = sel.size === 0;
  }
  $('#selectBtn').addEventListener('click', () => setSelMode(true));
  $('#selDone').addEventListener('click', () => setSelMode(false));
  $('#selAll').addEventListener('click', () => {
    const files = entries.filter(e => !e.dir);
    if (files.length && files.every(e => sel.has(e.name))) sel.clear(); else files.forEach(e => sel.add(e.name));
    $$('#grid .tile').forEach(t => t.classList.toggle('is-sel', sel.has(t.dataset.name)));
    updateSelBar();
  });
  $('#retryBtn').addEventListener('click', () => loadPath(curPath));
  $('#mkdirBtn').addEventListener('click', async () => {
    const name = await openModal({ title: 'New folder', msg: 'Create a folder in ' + curPath, showInput: true, placeholder: 'Folder name', okText: 'Create' });
    if (name === false) return;
    const n = String(name).trim();
    if (!n) return;
    if (n.includes('/')) { toast('Name cannot contain /', 'error'); return; }
    const r = await apiPost('/api/mkdir', { path: joinPath(curPath, n) });
    if (r.ok) { toast('Folder created'); loadPath(curPath); } else toast(r.error || 'Could not create folder', 'error');
  });

  async function downloadFile(name) {
    try { saveBlob(await fetchBlob(joinPath(curPath, name)), name); } catch (e) { toast('Download failed: ' + e.message, 'error'); }
  }
  function saveBlob(blob, name) {
    const a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = name;
    document.body.appendChild(a); a.click(); setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1500);
  }

  // --- Delete: one batch, so the frame remounts once -----------------------------------------
  let deleting = false, deleteBatchOpen = false;
  async function deleteNames(names) {
    if (!names.length) return false;
    userAction();
    const base = curPath;   // the folder the names were selected in, even if the user navigates away
    if (uploading || deleting) { toast('Wait for the current upload or delete to finish', 'error'); return false; }
    const hasDir = names.some(n => (entries.find(e => e.name === n) || {}).dir);
    const ok = await openModal({
      title: 'Delete ' + (names.length === 1 ? names[0] : plural(names.length, 'item')) + '?',
      msg: 'This cannot be undone.' + (hasDir ? ' Folders are deleted with everything inside them.' : ''), okText: 'Delete'
    });
    if (!ok) return false;
    if (uploading || deleting) { toast('Wait for the current upload or delete to finish', 'error'); return false; }
    deleting = true; hold(1);
    let done = 0, failed = 0, lastErr = '';
    showActivity('Deleting ' + plural(names.length, 'item') + '…', 'The frame will refresh when done', 0);
    try {
      deleteBatchOpen = true; await apiPost('/api/batch/begin');
      for (const n of names) {
        const r = await apiPost('/api/delete', { path: joinPath(base, n) });
        if (r.ok) done++; else { failed++; lastErr = r.error || 'failed'; }
        showActivity('Deleting ' + plural(names.length, 'item') + '…', (done + failed) + ' of ' + names.length + ' done. The frame will refresh when done.', (done + failed) / names.length);
      }
    } finally {
      await apiPost('/api/batch/end'); deleteBatchOpen = false;
      deleting = false; hold(-1); hideActivity();
    }
    sel.clear(); if (selMode) setSelMode(false);
    toast(failed ? 'Deleted ' + done + ', ' + failed + ' failed (' + lastErr + ')' : 'Deleted ' + plural(done, 'item'), failed ? 'error' : undefined);
    await loadPath(curPath); refreshStatus();
    return true;
  }
  $('#delBtn').addEventListener('click', () => deleteNames([...sel]));

  // --- Full-size viewer ------------------------------------------------------------------------
  let viewIdx = -1, viewList = [], viewUrl = null, viewTok = 0, viewBlob = null;
  function openViewer(name) {
    viewList = sortedEntries().filter(e => !e.dir && isImageName(e.name)).map(e => e.name);
    viewIdx = viewList.indexOf(name);
    if (viewIdx < 0) return;
    $('#viewer').classList.remove('hidden'); document.body.style.overflow = 'hidden';
    showViewerPhoto();
  }
  async function showViewerPhoto() {
    const tok = ++viewTok, name = viewList[viewIdx];
    $('#viewerName').textContent = name + ' (' + (viewIdx + 1) + '/' + viewList.length + ')';
    $('#viewerPrev').classList.toggle('hidden', viewIdx <= 0);
    $('#viewerNext').classList.toggle('hidden', viewIdx >= viewList.length - 1);
    const img = $('#viewerImg'), wait = $('#viewerWait');
    img.classList.add('hidden'); wait.textContent = 'Loading…'; wait.classList.remove('hidden');
    if (viewUrl) { URL.revokeObjectURL(viewUrl); viewUrl = null; }
    viewBlob = null; hold(1);
    try {
      const b = await fetchBlob(joinPath(curPath, name));
      if (tok !== viewTok) return;
      viewBlob = b; viewUrl = URL.createObjectURL(b);
      img.onerror = () => { wait.textContent = 'This browser cannot show this file.'; wait.classList.remove('hidden'); img.classList.add('hidden'); };
      img.onload = () => { wait.classList.add('hidden'); img.classList.remove('hidden'); };
      img.src = viewUrl;
    } catch (e) { if (tok === viewTok) wait.textContent = 'Could not load: ' + e.message; }
    finally { hold(-1); }
  }
  function closeViewer() {
    viewTok++; $('#viewer').classList.add('hidden'); document.body.style.overflow = '';
    $('#viewerImg').removeAttribute('src');
    if (viewUrl) { URL.revokeObjectURL(viewUrl); viewUrl = null; }
  }
  function stepViewer(d) { const n = viewIdx + d; if (n >= 0 && n < viewList.length) { viewIdx = n; showViewerPhoto(); } }
  $('#viewerClose').addEventListener('click', closeViewer);
  $('#viewerPrev').addEventListener('click', () => stepViewer(-1));
  $('#viewerNext').addEventListener('click', () => stepViewer(1));
  $('#viewerDl').addEventListener('click', () => { if (viewBlob) saveBlob(viewBlob, viewList[viewIdx]); });
  $('#viewerDel').addEventListener('click', async () => {
    const name = viewList[viewIdx];
    if (await deleteNames([name])) closeViewer();
  });
  document.addEventListener('keydown', e => {
    if ($('#viewer').classList.contains('hidden') || !$('#modal').classList.contains('hidden')) return;
    if (e.key === 'ArrowLeft') stepViewer(-1); else if (e.key === 'ArrowRight') stepViewer(1);
  });

  // --- Activity banner ---------------------------------------------------------------------------
  let activityTimer = null;
  function showActivity(title, sub, frac) {
    clearTimeout(activityTimer);
    $('#activityTitle').textContent = title; $('#activitySub').textContent = sub;
    $('#activityBar').style.width = Math.round(Math.max(0, Math.min(1, frac)) * 100) + '%';
    $('#activity').classList.remove('hidden');
  }
  function hideActivity() { clearTimeout(activityTimer); $('#activity').classList.add('hidden'); }

  // --- Upload ---------------------------------------------------------------------------------------
  // The frame re-reads the card only when the USB drive is reconnected, so a multi-file upload is one
  // batch: begin before the first file, every file in sequence, end once the queue is empty. Files added
  // while a batch is running join it.
  const queue = [];
  let uploading = false, batchOpen = false, batchNo = 0, freeLeft = Infinity, lastStamp = '', stampN = 0;

  function nextName() {
    const d = new Date();
    const stamp = d.getFullYear() + pad(d.getMonth() + 1) + pad(d.getDate()) + '_' + pad(d.getHours()) + pad(d.getMinutes()) + pad(d.getSeconds());
    stampN = stamp === lastStamp ? stampN + 1 : 1; lastStamp = stamp;
    // The counter restarts with every page load and the device replaces a file of the same name, so two
    // phones (or the fall-back DST hour) could overwrite each other within one second. A random suffix
    // keeps names unique; the timestamp still sorts them.
    const r = new Uint8Array(2); crypto.getRandomValues(r);
    return stamp + '_' + pad(stampN) + '_' + [...r].map(b => b.toString(16).padStart(2, '0')).join('') + '.jpg';
  }
  async function addFiles(fileList) {
    const all = [...(fileList || [])];
    if (!all.length) return;
    userAction();
    if (deleting) { toast('Wait for the delete to finish', 'error'); return; }
    const files = all.filter(f => (f.type && f.type.startsWith('image/')) || isImageName(f.name));
    if (files.length < all.length) toast('Skipped ' + plural(all.length - files.length, 'file') + ' that are not images', 'error');
    if (!files.length) return;
    if (!uploading) { try { renderStatus(await apiStatus()); } catch {} }
    const card = status && status.card;
    if (card && !card.present) { toast('No SD card in the frame adapter', 'error'); return; }
    if (card) {
      const pending = queue.filter(q => !q.done).reduce((a, q) => a + Math.min(q.file.size, EST_MAX_BYTES), 0);
      const need = files.reduce((a, f) => a + Math.min(f.size, EST_MAX_BYTES), 0) + pending + SPACE_MARGIN;
      const free = uploading ? Math.min(freeLeft, card.freeBytes) : card.freeBytes;
      if (need > free) { toast('Not enough space: about ' + fmtBytes(need) + ' needed, ' + fmtBytes(free) + ' free', 'error'); return; }
    }
    if (!queue.some(q => !q.done)) batchNo++;
    showView('add'); $('#addSummary').classList.add('hidden');
    const dir = curPath;
    for (const f of files) {
      const it = { id: Math.random().toString(36).slice(2), file: f, name: nextName(), dir, batch: batchNo, done: false, started: false, failed: false, frac: 0 };
      it.el = makeQueueEl(it); queue.push(it); $('#queue').appendChild(it.el);
    }
    updateActivity();
    pump();
  }
  function makeQueueEl(it) {
    const el = document.createElement('div'); el.className = 'qitem';
    el.innerHTML = '<div class="qitem__row"><span class="qitem__name"></span><span class="qitem__state">Queued</span></div><div class="track"><i></i></div>';
    el.querySelector('.qitem__name').textContent = it.file.name + ' (' + fmtBytes(it.file.size) + ')';
    return el;
  }
  function setState(it, text, cls) {
    it.el.querySelector('.qitem__state').textContent = text;
    it.el.classList.remove('ok', 'fail'); if (cls) it.el.classList.add(cls);
  }
  function setProgress(it, frac) {
    it.frac = frac;
    it.el.querySelector('.track i').style.width = Math.round(frac * 100) + '%';
    setState(it, Math.round(frac * 100) + '%');
    updateActivity();
  }
  function finish(it, ok, msg) {
    it.done = true; it.failed = !ok; it.frac = 1;
    it.el.querySelector('.track i').style.width = ok ? '100%' : '0';
    setState(it, ok ? 'Done' : msg, ok ? 'ok' : 'fail');
    if (!ok) {
      const b = document.createElement('button'); b.type = 'button'; b.className = 'btn'; b.textContent = 'Retry';
      b.addEventListener('click', () => {
        b.remove(); it.batch = queue.some(q => !q.done) ? batchNo : ++batchNo;
        it.done = false; it.started = false; it.failed = false; it.frac = 0;
        setState(it, 'Queued'); updateActivity(); pump();   // same folder as when it was queued
      });
      it.el.appendChild(b);
    }
  }
  function updateActivity() {
    const items = queue.filter(q => q.batch === batchNo), left = items.filter(q => !q.done);
    if (!items.length || !left.length) return;
    const doneN = items.length - left.length, cur = left.find(q => q.started);
    showActivity('Sending ' + plural(items.length, 'photo') + '…', doneN + ' of ' + items.length + ' done. The frame will refresh when done.', (doneN + (cur ? cur.frac : 0)) / items.length);
  }

  function xhrUpload(it, blob) {
    return new Promise(resolve => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/upload?path=' + encodeURIComponent(it.dir));
      const ah = authHeader(); if (ah) xhr.setRequestHeader('Authorization', ah);
      it.sentAuth = ah;
      xhr.timeout = 180000;
      xhr.upload.onprogress = e => { if (e.lengthComputable) setProgress(it, e.loaded / e.total); };
      xhr.onload = () => {
        let j = null; try { j = JSON.parse(xhr.responseText); } catch {}
        if (xhr.status === 401) authRejected(it.sentAuth); else if (xhr.status >= 200 && xhr.status < 300) authAccepted(it.sentAuth);
        resolve(xhr.status >= 200 && xhr.status < 300 && j && j.ok ? { ok: true } : { ok: false, status: xhr.status, error: (j && j.error) || 'HTTP ' + xhr.status });
      };
      xhr.onerror = () => resolve({ ok: false, error: 'Network error' });
      xhr.ontimeout = () => resolve({ ok: false, error: 'Timed out' });
      const fd = new FormData(); fd.append('file', blob, it.name); xhr.send(fd);
    });
  }
  async function processItem(it) {
    it.started = true; setState(it, 'Preparing…'); updateActivity();
    let blob;
    try { blob = await reencode(it.file, MAX_EDGE, JPEG_QUALITY); }
    catch { finish(it, false, 'Could not read this image'); return; }
    if (blob.size + SPACE_MARGIN > freeLeft) { finish(it, false, 'Not enough space'); return; }
    setProgress(it, 0);
    let res = useMock ? await mockUpload(it, blob) : await xhrUpload(it, blob);
    if (!res.ok && res.status === 401 && await handleAuthFail(it.sentAuth)) res = await xhrUpload(it, blob);
    if (res.ok) { freeLeft -= blob.size; finish(it, true); } else finish(it, false, res.error || 'Failed');
  }
  async function pump() {
    if (uploading) return;
    uploading = true; hold(1);
    let sent = 0, failed = 0, total = 0;
    try {
      let next;
      while ((next = queue.find(q => !q.done && !q.started))) {
        if (!batchOpen) {
          batchOpen = true;
          await apiPost('/api/batch/begin');
          try { const s = await apiStatus(); renderStatus(s); freeLeft = s.card && s.card.present ? s.card.freeBytes : Infinity; } catch {}
        }
        try { await processItem(next); } catch (e) { finish(next, false, 'Error'); }
      }
    } finally {
      if (batchOpen) { batchOpen = false; await apiPost('/api/batch/end'); }
      const items = queue.filter(q => q.batch === batchNo);
      total = items.length; failed = items.filter(q => q.failed).length; sent = total - failed;
      uploading = false; hold(-1);
    }
    if (total) {
      const msg = 'Sent ' + plural(sent, 'photo') + (failed ? ', ' + failed + ' failed' : '') + (sent ? '. The frame is refreshing.' : '.');
      const s = $('#addSummary'); s.textContent = msg; s.classList.remove('hidden');
      showActivity(msg, '', 1); activityTimer = setTimeout(hideActivity, 7000);
    }
    refreshStatus(); loadPath(curPath);
    if (queue.some(q => !q.done && !q.started)) pump();   // added while the batch was closing
  }
  $('#clearDone').addEventListener('click', () => {
    for (let i = queue.length - 1; i >= 0; i--) if (queue[i].done) { queue[i].el.remove(); queue.splice(i, 1); }
    $('#addSummary').classList.add('hidden');
  });

  $('#fileInput').addEventListener('change', e => { const f = [...e.target.files]; e.target.value = ''; addFiles(f); });
  let dragDepth = 0;
  const isFileDrag = e => { try { return e.dataTransfer && [...e.dataTransfer.types].includes('Files'); } catch { return false; } };
  window.addEventListener('dragenter', e => { if (isFileDrag(e)) { e.preventDefault(); dragDepth++; $('#dropOverlay').classList.remove('hidden'); } });
  window.addEventListener('dragover', e => { if (isFileDrag(e)) e.preventDefault(); });
  window.addEventListener('dragleave', e => { if (isFileDrag(e) && --dragDepth <= 0) { dragDepth = 0; $('#dropOverlay').classList.add('hidden'); } });
  window.addEventListener('drop', e => {
    if (!isFileDrag(e)) return;
    e.preventDefault(); dragDepth = 0; $('#dropOverlay').classList.add('hidden');
    addFiles(e.dataTransfer.files);
  });

  // --- Restart slideshow ---------------------------------------------------------------------------------
  // A device restart is what reliably starts the frame's slideshow: the frame sees the drive plugged in again.
  $('#restartBtn').addEventListener('click', async () => {
    if (uploading || deleting) { toast('Wait for the current upload or delete to finish', 'error'); return; }
    if (!await openModal({ title: 'Restart the photo frame drive?', msg: 'The device restarts and the frame starts its slideshow again. An upload from another phone or the Shortcut would be cut off. The page is unavailable for about 10 seconds.', okText: 'Restart' })) return;
    userAction();
    const b = $('#restartBtn'); b.disabled = true;
    const r = await apiPost('/api/restart');
    if (!r.ok) { b.disabled = false; toast(r.error || 'Restart failed', 'error'); return; }
    toast('Restarting. The slideshow should start in about 10 seconds');
    setTimeout(() => { b.disabled = false; refreshStatus(); }, 12000);
  });

  // --- Wi-Fi ---------------------------------------------------------------------------------------------------
  $('#useStatic').addEventListener('change', e => $('#staticFields').classList.toggle('hidden', !e.target.checked));
  $('#scanBtn').addEventListener('click', async () => {
    const btn = $('#scanBtn'), list = $('#scanList'); btn.disabled = true; $('#scanStatus').textContent = 'Scanning…'; list.textContent = '';
    try {
      const nets = ((await apiWifiScan()).networks || []).sort((a, b) => b.rssi - a.rssi);
      nets.forEach(n => {
        const row = document.createElement('button'); row.type = 'button'; row.className = 'scanitem';
        const a = document.createElement('span'); a.textContent = n.ssid || '(hidden)';
        const b = document.createElement('span'); b.className = 'muted small'; b.textContent = (n.secure ? 'secured, ' : 'open, ') + n.rssi + ' dBm';
        row.append(a, b);
        row.addEventListener('click', () => { $('#wifiSsid').value = n.ssid; $('#wifiPass').focus(); });
        list.appendChild(row);
      });
      $('#scanStatus').textContent = nets.length ? 'Tap a network to use it.' : 'No networks found.';
    } catch (e) { $('#scanStatus').textContent = ''; toast('Scan failed: ' + e.message, 'error'); }
    finally { btn.disabled = false; }
  });
  $('#wifiForm').addEventListener('submit', async e => {
    e.preventDefault();
    const fd = new FormData(e.target);
    const data = { ssid: fd.get('ssid') || '', password: fd.get('password') || '', hostname: fd.get('hostname') || '', useStatic: $('#useStatic').checked ? '1' : '0',
      ip: fd.get('ip') || '', gw: fd.get('gw') || '', mask: fd.get('mask') || '', dns: fd.get('dns') || '' };
    if (!data.ssid) { toast('Enter the network name', 'error'); return; }
    if (!await openModal({ title: 'Save Wi-Fi settings?', msg: 'The frame adapter restarts and joins "' + data.ssid + '". You may need to reconnect to reach this page.', okText: 'Save and restart' })) return;
    const btn = e.target.querySelector('button[type="submit"]'); btn.disabled = true;
    const r = await apiPost('/api/wifi', data);
    btn.disabled = false;
    if (r.ok) toast('Saved. Restarting…'); else toast(r.error || 'Save failed', 'error');
  });

  // --- Login --------------------------------------------------------------------------------------------------
  $('#authForm').addEventListener('submit', async e => {
    e.preventDefault();
    const user = $('#authUser').value.trim(), p1 = $('#authPass').value, p2 = $('#authPass2').value;
    if (!user || !p1) { toast('Enter a username and password', 'error'); return; }
    if (p1 !== p2) { toast('Passwords do not match', 'error'); return; }
    if (!await openModal({ title: 'Save login?', msg: 'From now on this page asks for "' + user + '" and the new password.', okText: 'Save' })) return;
    const r = await apiPost('/api/auth/set', { user, pass: p1 });
    if (r.ok) {
      // The device just accepted this login, so save it now rather than waiting for the next request.
      setAuth(user, p1); authUnverified = false; store.set('authUser', user); store.set('authPass', p1);
      $('#authPass').value = ''; $('#authPass2').value = ''; $('#authStatus').textContent = 'Saved'; toast('Login saved');
    }
    else toast(r.error || 'Save failed', 'error');
  });

  // --- Firmware (OTA) --------------------------------------------------------------------------------------------
  $('#otaFile').addEventListener('change', async e => {
    const f = e.target.files[0]; if (!f) return;
    $('#otaFileName').textContent = f.name + ' (' + fmtBytes(f.size) + ')';
    if (!await openModal({ title: 'Flash firmware?', msg: f.name + ', ' + fmtBytes(f.size) + '. The frame adapter restarts afterwards.', okText: 'Flash' })) { e.target.value = ''; return; }
    if (useMock) { toast('Preview mode: nothing was flashed'); e.target.value = ''; return; }
    const bar = $('#otaBar'); $('#otaProgress').classList.remove('hidden'); bar.style.width = '0';
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota');
    const ah = authHeader(); if (ah) xhr.setRequestHeader('Authorization', ah);
    xhr.upload.onprogress = ev => { if (ev.lengthComputable) bar.style.width = Math.round(ev.loaded / ev.total * 100) + '%'; };
    xhr.onload = () => {
      let j = null; try { j = JSON.parse(xhr.responseText); } catch {}
      if (xhr.status >= 200 && xhr.status < 300 && j && j.ok) { toast('Firmware written. Restarting…'); bar.style.width = '100%'; }
      else if (xhr.status === 401) { handleAuthFail(ah); toast('Log in, then choose the file again', 'error'); }
      else toast((j && j.error) || 'Update failed (HTTP ' + xhr.status + ')', 'error');
      e.target.value = '';
    };
    xhr.onerror = () => { toast('Update failed: network error', 'error'); e.target.value = ''; };
    const fd = new FormData(); fd.append('file', f, f.name); xhr.send(fd);
  });
  $('#otaSDTrigger').addEventListener('click', async () => {
    const b = $('#otaSDTrigger'); b.disabled = true;
    const r = await apiPost('/api/ota/sd');
    b.disabled = false;
    if (r.ok) toast('Update started. Restarting…'); else toast(r.error || 'Update failed', 'error');
  });

  // --- Init -------------------------------------------------------------------------------------------------------
  // Sample data only with ?mock. Falling back to it when the device did not answer once made every
  // later upload "succeed" against the mock while nothing reached the frame.
  async function init() {
    if (FORCE_MOCK) {
      useMock = true;
      renderStatus(await apiStatus());
      toast('Preview mode: sample data, no device');
    } else {
      for (let tries = 0; ; tries++) {
        try {
          const s = await apiStatus();
          renderStatus(s);
          if (s.auth && s.auth.user && !$('#authUser').value) $('#authUser').value = s.auth.user;
          break;
        } catch (err) {
          console.warn('[Photo Frame] /api/status failed, retrying', err);
          if (tries === 0) toast('Cannot reach the photo frame. Retrying…', 'error');
          setChip('#chipWifi', 'err', 'Offline');
          await sleep(POLL_MS);
        }
      }
    }
    await loadPath('/');
    startPolling();
  }
  // A batch left open when the page goes away would keep the frame from picking up the new photos for
  // 10 minutes. keepalive lets the request finish after the page is gone.
  // A page restored from the back-forward cache already sent its batch end in pagehide.
  addEventListener('pageshow', e => { if (e.persisted) { batchOpen = false; deleteBatchOpen = false; } });
  addEventListener('pagehide', () => {
    if ((!batchOpen && !deleteBatchOpen) || useMock) return;
    try { fetch('/api/batch/end', { method: 'POST', keepalive: true, headers: withAuthHeaders() }); } catch {}
  });
  init();
})();
