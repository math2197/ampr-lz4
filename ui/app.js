(function () {
'use strict';
var $ = function (id) { return document.getElementById(id); };
var S = { games: [], info: null, filter: 'all', status: null, loaded: false, error: null,
  sel: null, dest: null, level: 9, lastJob: null, timer: null, histOpen: false, busy: false };
var STAGES = ['mount', 'scan', 'pack', 'copy', 'verify', 'finish'];
var STAGE_PT = { mount: 'Montando imagem', scan: 'Analisando arquivos', pack: 'Comprimindo',
  copy: 'Copiando arquivos', verify: 'Verificando', finish: 'Finalizando', done: 'Concluído',
  error: 'Erro', cancelled: 'Cancelado' };
var DRIVES = { data: ['Interno', '/data'], ext0: ['M.2', 'ext0'], ext1: ['M.2', 'ext1'] };
var LEVELS = [
  { l: 4, n: 'Rápida', d: 'Menos tempo, economiza menos espaço' },
  { l: 9, n: 'Equilibrada', d: 'Bom equilíbrio (recomendado)' },
  { l: 12, n: 'Máxima', d: 'Mais lenta, menor tamanho' }];
var PAL = [['#0a4d9c', '#3b9cff'], ['#5b2a86', '#c06cf0'], ['#0e6b5c', '#35d0a8'], ['#8a3a1d', '#ff9a5a'],
  ['#7a1f4a', '#ff6fa5'], ['#2d4a8a', '#7aa5ff'], ['#5a6b14', '#c4e04a']];

function esc(s) { return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) {
  return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]; }); }
function gb(b) { return (b / 1073741824).toFixed(b >= 10737418240 ? 1 : 2).replace('.', ',') + ' GB'; }
function mbs(b) { return (b / 1048576).toFixed(1).replace('.', ',') + ' MB/s'; }
function dur(s) { s = Math.max(0, Math.round(s)); var h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60);
  return h ? h + ' h ' + m + ' min' : m ? m + ' min ' + (s % 60) + ' s' : s + ' s'; }
function hash(s) { var h = 0; for (var i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0; return Math.abs(h); }
function initials(t) { var w = String(t).replace(/[^A-Za-z0-9À-ÿ ]/g, ' ').split(/\s+/).filter(Boolean);
  return ((w[0] || '?').charAt(0) + (w.length > 1 ? w[1].charAt(0) : (w[0] || '').charAt(1))).toUpperCase(); }
function api(method, url, body) {
  var o = { method: method, cache: 'no-store' };
  if (body) { o.headers = { 'Content-Type': 'application/json' }; o.body = JSON.stringify(body); }
  return fetch(url, o).then(function (r) { return r.json().catch(function () { return { ok: false, error: 'Resposta inválida (HTTP ' + r.status + ')' }; }); });
}
function toast(msg, err) {
  var d = document.createElement('div'); d.className = 'toast' + (err ? ' err' : ''); d.textContent = msg;
  $('toasts').appendChild(d);
  setTimeout(function () { if (d.parentNode) d.parentNode.removeChild(d); }, err ? 9000 : 6000);
}
function cover(g, big, extra) {
  var p = PAL[hash(g.title_id || g.id) % PAL.length];
  return '<div class="cover' + (big ? ' big' : '') + '"><div class="ph" style="background:linear-gradient(135deg,' + p[0] + ',' + p[1] + ')">' + esc(initials(g.title)) + '</div>' +
    '<img alt="" loading="lazy" src="/api/icon?id=' + encodeURIComponent(g.id) + '" onerror="this.parentNode.removeChild(this)">' + (extra || '') + '</div>';
}
function jobState(id) {
  var s = S.status; if (!s) return null;
  if (s.current && s.current.id === id && !/^(done|error|cancelled)$/.test(s.current.stage)) return 'run';
  for (var i = 0; i < (s.queue || []).length; i++) if (s.queue[i].id === id) return 'q';
  return null;
}
function find(id) { for (var i = 0; i < S.games.length; i++) if (S.games[i].id === id) return S.games[i]; return null; }

/* ---------- header ---------- */
function renderHeader() {
  var i = S.info, p = $('smp');
  if (!i) return;
  $('ver').textContent = 'v' + i.version;
  if (i.smp_version) { p.className = 'pill pill-ok'; p.textContent = 'ShadowMountPlus ' + i.smp_version; }
  else { p.className = 'pill pill-bad'; p.textContent = 'SMP não encontrado'; }
}

/* ---------- grid ---------- */
function visible() {
  return S.games.filter(function (g) { return S.filter === 'all' || (S.filter === 'done') === !!g.converted; });
}
function renderGrid() {
  var key = [S.filter, S.loaded, S.error, S.games.map(function (g) { return g.id + g.converted + g.compatible + (jobState(g.id) || ''); }).join()].join('|');
  if (key === S.gridKey) return;
  var anim = S.gridFilter !== S.filter || !S.gridKey; S.gridKey = key; S.gridFilter = S.filter;
  var all = S.games, done = all.filter(function (g) { return g.converted; }).length;
  $('cAll').textContent = all.length; $('cDone').textContent = done; $('cTodo').textContent = all.length - done;
  var st = $('state'), grid = $('grid'), list = visible();
  var chips = document.querySelectorAll('.chipbtn');
  for (var c = 0; c < chips.length; c++) chips[c].className = 'chipbtn' + (chips[c].getAttribute('data-f') === S.filter ? ' on' : '');
  if (S.error) {
    grid.innerHTML = ''; st.hidden = false;
    st.innerHTML = '<svg viewBox="0 0 24 24"><path d="M12 8v5m0 3v.01M10.3 3.9L2.4 18a2 2 0 0 0 1.7 3h15.8a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></svg><h2>Não foi possível carregar os jogos</h2><div>' + esc(S.error) + '</div><button type="button" class="btn btn-primary" data-act="reload">Tentar de novo</button>';
    return;
  }
  if (!S.loaded) { grid.innerHTML = ''; st.hidden = false; st.innerHTML = '<div>Carregando jogos…</div>'; return; }
  if (!list.length) {
    grid.innerHTML = ''; st.hidden = false;
    st.innerHTML = '<svg viewBox="0 0 24 24"><rect x="3" y="6" width="18" height="12" rx="3" fill="none" stroke="currentColor" stroke-width="2"/><path d="M7 12h4M9 10v4M15 11h.01M17 13h.01" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></svg><h2>' +
      (all.length ? 'Nenhum jogo neste filtro' : 'Nenhum jogo encontrado') + '</h2><div>' +
      (all.length ? 'Escolha outro filtro acima.' : 'Nenhuma imagem .exfat ou .ffpfsc foi encontrada nos discos.') + '</div>';
    return;
  }
  st.hidden = true;
  var h = '';
  list.forEach(function (g, i) {
    var js = jobState(g.id), b = '';
    if (js === 'run') b += '<span class="badge b-run">Convertendo</span>';
    else if (js === 'q') b += '<span class="badge b-q">Na fila</span>';
    if (g.converted) b += '<span class="badge b-ok">Convertido</span>';
    if (g.compatible === false) b += '<span class="badge b-gray">Incompatível</span>';
    var d = DRIVES[g.drive] || [g.drive, ''];
    h += '<button type="button" class="card' + (anim ? ' in' : '') + '" data-id="' + esc(g.id) + '" style="animation-delay:' + Math.min(i, 12) * 30 + 'ms">' +
      cover(g, false, '<div class="badges">' + b + '</div>') +
      '<div class="txt"><div class="ct">' + esc(g.title) + '</div><div class="cid">' + esc(g.title_id) + '</div>' +
      '<div class="meta"><span class="chip">.' + esc(g.format) + '</span><span class="chip">' + esc(d[0]) + '</span><span class="chip sz">' + gb(g.size_bytes) + '</span></div></div></button>';
  });
  var fid = document.activeElement && document.activeElement.getAttribute && document.activeElement.getAttribute('data-id');
  grid.innerHTML = h;
  if (fid) { var el = grid.querySelector('[data-id="' + fid.replace(/"/g, '') + '"]'); if (el) el.focus(); }
}

/* ---------- modal ---------- */
function opts(el, items, cur, name) {
  el.innerHTML = items.map(function (o) {
    return '<button type="button" role="radio" class="opt" data-' + name + '="' + o.v + '" aria-checked="' + (o.v == cur) + '"><b>' + esc(o.n) + '</b><small>' + esc(o.d) + '</small></button>';
  }).join('');
}
function renderModal() {
  var g = S.sel; if (!g) return;
  var drives = []; S.games.forEach(function (x) { if (drives.indexOf(x.drive) < 0) drives.push(x.drive); });
  ['data'].forEach(function (d) { if (drives.indexOf(d) < 0) drives.push(d); });
  drives.sort();
  if (!S.dest || drives.indexOf(S.dest) < 0) S.dest = drives.indexOf(g.drive) >= 0 ? g.drive : drives[0];
  $('mCover').outerHTML = cover(g, true).replace('class="cover big"', 'class="cover big" id="mCover"');
  $('mTitle').textContent = g.title;
  var d = DRIVES[g.drive] || [g.drive, ''];
  $('mMeta').innerHTML = '<span class="chip">' + esc(g.title_id) + '</span><span class="chip">.' + esc(g.format) + '</span><span class="chip">' + esc(d[0] + ' ' + d[1]) + '</span><span class="chip">' + gb(g.size_bytes) + '</span>' +
    (g.compatible === false ? '<span class="chip">Incompatível</span>' : '');
  opts($('mDest'), drives.map(function (x) { var q = DRIVES[x] || [x, '']; return { v: x, n: q[0] + ' ' + q[1], d: x === 'data' ? 'Armazenamento interno' : 'Disco M.2' }; }), S.dest, 'dest');
  opts($('mLevel'), LEVELS.map(function (x) { return { v: x.l, n: x.n, d: x.d }; }), S.level, 'level');
  var done = $('mDone');
  if (g.converted) { done.hidden = false; done.textContent = 'Já convertido' + (g.dest_path ? ': ' + g.dest_path : '') + '. Reconverter exige que o destino seja removido.'; }
  else done.hidden = true;
  var js = jobState(g.id);
  $('mGo').textContent = js ? (js === 'run' ? 'Convertendo…' : 'Na fila') : (g.converted ? 'Converter novamente' : 'Converter');
  $('mGo').disabled = !!js || S.busy;
}
function openModal(id) {
  var g = find(id); if (!g) return;
  S.sel = g; S.opener = document.activeElement; $('mErr').hidden = true;
  $('modal').hidden = false; renderModal();
  var f = $('mGo'); if (f.disabled) f = $('mCancel'); f.focus();
}
function closeModal() {
  $('modal').hidden = true; S.sel = null;
  if (S.opener && document.body.contains(S.opener)) S.opener.focus();
}
function convert() {
  var g = S.sel; if (!g || S.busy) return;
  if (g.converted && !S.confirmed) {
    ask('Reconverter ' + g.title + '?', 'Já existe uma versão convertida. O servidor recusa se o destino existir.', 'Reconverter', function () { S.confirmed = true; convert(); });
    return;
  }
  S.confirmed = false; S.busy = true; renderModal(); $('mErr').hidden = true;
  api('POST', '/api/convert', { id: g.id, dest_drive: S.dest, level: S.level }).then(function (r) {
    S.busy = false;
    if (r && r.ok) { toast('Conversão adicionada à fila.'); closeModal(); poll(); }
    else { var e = $('mErr'); e.hidden = false; e.textContent = (r && r.error) || 'Falha ao iniciar a conversão.'; renderModal(); }
  }).catch(function () { S.busy = false; var e = $('mErr'); e.hidden = false; e.textContent = 'Sem conexão com o PS5.'; renderModal(); });
}

/* ---------- confirm ---------- */
function ask(title, text, yes, fn) {
  S.askFn = fn; S.askOpener = document.activeElement;
  $('cTitle').textContent = title; $('cText').textContent = text; $('cYes').textContent = yes;
  $('confirm').hidden = false; $('cNo').focus();
}
function closeAsk() { $('confirm').hidden = true; S.askFn = null;
  var o = S.askOpener; if (o && document.body.contains(o)) o.focus(); }

/* ---------- job bar ---------- */
function active(c) { return c && !/^(done|error|cancelled)$/.test(c.stage); }
function renderJob() {
  var s = S.status, c = s && s.current, on = active(c);
  $('job').hidden = !on; document.body.className = on ? 'hasjob' : '';
  if (!on) return;
  var i = STAGES.indexOf(c.stage);
  $('jTitle').textContent = c.title;
  $('jQueue').textContent = s.queue && s.queue.length ? '+' + s.queue.length + ' na fila' : '';
  $('jSteps').innerHTML = STAGES.map(function (k, n) { return '<li class="' + (n < i ? 'done' : n === i ? 'cur' : '') + '">' + STAGE_PT[k] + '</li>'; }).join('');
  var pct = c.bytes_total > 0 ? Math.min(100, c.bytes_done * 100 / c.bytes_total) : 0;
  $('jBar').style.width = pct.toFixed(1) + '%';
  $('jStage').textContent = (STAGE_PT[c.stage] || c.stage) + '…';
  $('jPct').textContent = pct.toFixed(0) + '%';
  $('jSpeed').textContent = c.speed_bps > 0 ? mbs(c.speed_bps) : '';
  $('jEta').textContent = c.eta_s > 0 ? 'Restam ' + dur(c.eta_s) : '';
  $('jSize').textContent = c.src_bytes > 0 && c.out_bytes > 0 ?
    gb(c.src_bytes) + ' → ' + gb(c.out_bytes) + ' (−' + Math.max(0, Math.round((1 - c.out_bytes / c.src_bytes) * 100)) + '%)' : '';
}
function hres(x) { return x.result || x.stage; } /* servidor envia "result"; mock antigo, "stage" */
function renderHist() {
  var h = ((S.status && S.status.history) || []).slice().sort(function (a, b) { return b.job - a.job; });
  $('hist').hidden = !h.length;
  $('histList').innerHTML = h.map(function (x) {
    var st = hres(x);
    var r = st === 'done' ? (x.src_bytes ? gb(x.src_bytes) + ' → ' + gb(x.out_bytes) + ' (−' + Math.round((1 - x.out_bytes / x.src_bytes) * 100) + '%)' : 'Concluído') :
      st === 'cancelled' ? 'Cancelado' : (x.error || 'Erro');
    return '<li><span class="dot ' + esc(st) + '"></span><span class="ht">' + esc(x.title) + '</span><span class="hs">' + esc(r) + '</span></li>';
  }).join('');
}
function handleFinished(prev, st) {
  var h = st.history || [];
  if (!prev) return;
  var changed = false;
  h.forEach(function (x) {
    if (x.job > S.lastJob && !S.seen[x.job]) {
      S.seen[x.job] = 1; changed = true;
      var st = hres(x);
      if (st === 'done') {
        var sv = x.src_bytes ? ' Economia de ' + gb(Math.max(0, x.src_bytes - x.out_bytes)) + ' (−' + Math.round((1 - x.out_bytes / x.src_bytes) * 100) + '%).' : '';
        toast('Concluído: ' + x.title + '.' + sv);
      } else if (st === 'error') toast('Erro em ' + x.title + ': ' + (x.error || 'falha desconhecida'), true);
      else if (st === 'cancelled') toast('Cancelado: ' + x.title, true);
    }
  });
  if (changed) loadGames();
}
function poll() {
  clearTimeout(S.timer);
  api('GET', '/api/status').then(function (st) {
    var prev = S.status; S.status = st;
    if (!S.seen) { S.seen = {}; S.lastJob = 0; (st.history || []).forEach(function (x) { S.seen[x.job] = 1; }); }
    else handleFinished(prev, st);
    renderJob(); renderHist(); renderGrid(); if (S.sel) renderModal();
    S.timer = setTimeout(poll, active(st.current) || (st.queue && st.queue.length) ? 1000 : 5000);
  }).catch(function () { S.timer = setTimeout(poll, 5000); });
}
function loadGames() {
  var b = $('btnRefresh'); b.className = 'btn btn-ghost spinning';
  return api('GET', '/api/games').then(function (r) {
    S.games = r.games || []; S.error = r.games ? null : (r.error || 'Resposta inesperada'); S.loaded = true;
  }).catch(function () { S.error = 'Sem conexão com o PS5. Verifique a rede.'; S.loaded = true; })
    .then(function () { b.className = 'btn btn-ghost'; renderGrid(); if (S.sel) renderModal(); });
}
function loadInfo() { api('GET', '/api/info').then(function (i) { S.info = i; renderHeader(); }).catch(function () {}); }

/* ---------- eventos ---------- */
document.addEventListener('click', function (e) {
  var t = e.target; while (t && t !== document && t.nodeType === 1) {
    if (t.getAttribute('data-act') === 'reload') return loadGames();
    if (t.className && /\bcard\b/.test(t.className) && t.getAttribute('data-id')) return openModal(t.getAttribute('data-id'));
    if (t.getAttribute('data-dest')) { S.dest = t.getAttribute('data-dest'); renderModal(); return $('mDest').querySelector('[data-dest="' + S.dest + '"]').focus(); }
    if (t.getAttribute('data-level')) { S.level = +t.getAttribute('data-level'); renderModal(); return $('mLevel').querySelector('[data-level="' + S.level + '"]').focus(); }
    if (t.getAttribute('data-f')) { S.filter = t.getAttribute('data-f'); renderGrid(); var n = document.querySelector('[data-f="' + S.filter + '"]'); if (n) n.focus(); return; }
    t = t.parentNode;
  }
});
$('btnRefresh').onclick = function () { loadInfo(); loadGames(); };
$('histToggle').onclick = function () { S.histOpen = !S.histOpen; this.setAttribute('aria-expanded', S.histOpen); $('histList').hidden = !S.histOpen; };
$('mClose').onclick = $('mCancel').onclick = $('scrim').onclick = closeModal;
$('mGo').onclick = convert;
$('cNo').onclick = closeAsk;
$('cYes').onclick = function () { var f = S.askFn; closeAsk(); if (f) f(); };
$('jCancel').onclick = function () {
  var c = S.status && S.status.current; if (!c) return;
  ask('Cancelar conversão?', 'O progresso de "' + c.title + '" será perdido.', 'Cancelar conversão', function () {
    api('POST', '/api/cancel', { job: c.job }).then(poll);
  });
};

/* navegação espacial por setas */
function scope() { return !$('confirm').hidden ? $('confirm') : !$('modal').hidden ? $('modal') : document; }
function focusables() {
  var all = scope().querySelectorAll('button:not([disabled])'), out = [];
  for (var i = 0; i < all.length; i++) { var r = all[i].getBoundingClientRect(); if (r.width > 0 && r.height > 0) out.push(all[i]); }
  return out;
}
function move(dir) {
  var cur = document.activeElement, list = focusables();
  if (!cur || list.indexOf(cur) < 0) { if (list[0]) list[0].focus(); return; }
  var a = cur.getBoundingClientRect(), ax = a.left + a.width / 2, ay = a.top + a.height / 2, best = null, bs = 1e12;
  list.forEach(function (el) {
    if (el === cur) return;
    var r = el.getBoundingClientRect(), x = r.left + r.width / 2, y = r.top + r.height / 2, dx = x - ax, dy = y - ay, main, cross;
    if (dir === 'l') { if (r.right > a.left + 4 + a.width * .5 && dx >= -4) return; main = -dx; cross = Math.abs(dy); }
    else if (dir === 'r') { if (dx <= 4) return; main = dx; cross = Math.abs(dy); }
    else if (dir === 'u') { if (dy >= -4) return; main = -dy; cross = Math.abs(dx); }
    else { if (dy <= 4) return; main = dy; cross = Math.abs(dx); }
    if (main <= 0) return;
    var sc = main + cross * 3;
    if (sc < bs) { bs = sc; best = el; }
  });
  if (best) { best.focus(); try { best.scrollIntoView({ block: 'nearest' }); } catch (x) { best.scrollIntoView(); } }
}
document.addEventListener('keydown', function (e) {
  var k = e.keyCode;
  if (k >= 37 && k <= 40) { e.preventDefault(); move(['l', 'u', 'r', 'd'][k - 37]); }
  else if (k === 27 || k === 8 || k === 461) {
    if (!$('confirm').hidden) { e.preventDefault(); closeAsk(); }
    else if (!$('modal').hidden) { e.preventDefault(); closeModal(); }
  }
});

renderGrid(); loadInfo(); loadGames(); poll();
})();
