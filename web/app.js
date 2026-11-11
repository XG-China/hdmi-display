'use strict';

/* The panel is served both directly (http://nas:8080/) and through the fnOS
 * gateway (/app/hdmi-display/).  Build every API and preview URL relative to
 * the page directory so both work. */
const TOKEN = new URLSearchParams(location.search).get('token') || '';
const BASE = location.pathname.endsWith('/')
  ? location.pathname
  : location.pathname.replace(/[^/]*$/, '');

const $ = (id) => document.getElementById(id);
/* Only these sources consume keyboard/mouse events. */
const BINDABLE = new Set(['remote', 'rdp', 'vnc', 'web']);
const NAMES = {};
const LASTT = {};
const LASTA = {};
const DIRTY = {};   /* user is editing this card; do not clobber with state */
const PENDING = {}; /* applied values waiting for the backend state to catch up */

/* Per-output, per-source-type last applied value. Kept in localStorage so
 * switching the dropdown restores the last address used for that screen. */
const HIST_KEY = 'hdmiDisplay.history.v1';
let HIST = {};
try { HIST = JSON.parse(localStorage.getItem(HIST_KEY) || '{}') || {}; }
catch (e) { HIST = {}; }

function saveHist() {
  try { localStorage.setItem(HIST_KEY, JSON.stringify(HIST)); } catch (e) {}
}

function remember(name, type, arg) {
  if (!name || !type || type === 'ui' || type === 'bars') return;
  if (!HIST[name]) HIST[name] = {};
  if (arg) {
    HIST[name][type] = arg;
    saveHist();
  }
}

function histFor(name, type) {
  return (name && HIST[name] && HIST[name][type]) || '';
}

function stripProto(value, proto) {
  return String(value || '').replace(new RegExp('^' + proto + ':(//)?', 'i'), '');
}

function onArgInput(i) {
  const a = document.getElementById('arg-' + i);
  DIRTY[i] = 1;
  PENDING[i] = null;
  if (a) LASTA[i] = a.value;
  syncVncSelect(i);
}

function onTypeChange(i) {
  DIRTY[i] = 1;
  PENDING[i] = null;
  syncType(i);
}
let sig = '';
let INDEVS = [];
let INROWS = [];
let IN_SIG = '';
let SHOWALL = false;
let LASTSTATE = null;
let refreshTimer = null;
let VNC_HOSTS = [];
let VNC_SIG = '';

function esc(v) {
  return String(v == null ? '' : v).replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  })[c]);
}

function withToken(u) {
  if (!TOKEN) return u;
  return u + (u.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(TOKEN);
}

function api(path) {
  return withToken(BASE + path);
}

function noCache(u) {
  return u + (u.includes('?') ? '&' : '?') + '_=' + Date.now();
}

function fmtUp(s) {
  s = Math.max(0, Math.floor(s || 0));
  const d = Math.floor(s / 86400); s -= d * 86400;
  const h = Math.floor(s / 3600); s -= h * 3600;
  const m = Math.floor(s / 60); s -= m * 60;
  return (d ? d + '天 ' : '') + h + ':' + String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0');
}

let toastTimer = null;
function toast(msg) {
  const el = $('toast');
  if (!el) return;
  el.textContent = msg;
  el.classList.remove('hidden');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.add('hidden'), 2600);
}

function syncType(i) {
  const t = document.getElementById('type-' + i);
  const a = document.getElementById('arg-' + i);
  if (!t || !a) return;
  const v = t.value;
  const ph = {
    media: 'URL / 文件 / 摄像头地址',
    image: '图片路径或 URL',
    vnc: 'VNC:主机:端口 或 VNC:/var/run/vms/xxx.vnc.sock',
    web: 'https://example.com',
    remote: '自动识别：用户:密码@主机[:端口]（RDP）｜主机:端口、socket 或路径（VNC）',
    rdp: 'RDP:用户:密码@主机[:端口]（默认控制台；?normal 独立会话）',
    screen: '留空自动；也可填 fb0 或 :0.0',
    pip: '主源|小源|x,y,w,h',
  };
  const noarg = (v === 'ui' || v === 'bars');
  const prev = t.dataset.prev;
  if (prev && prev !== v) {
    a.value = histFor(NAMES[i], v);
    LASTA[i] = a.value;
  }
  t.dataset.prev = v;
  a.disabled = noarg;
  a.placeholder = noarg ? '该类型不需要参数' : (ph[v] || '参数');
  if (noarg) a.value = '';
  const row = document.getElementById('vncrow-' + i);
  if (row) row.style.display = (v === 'remote') ? '' : 'none';
  if (v === 'remote') syncVncSelect(i);
}

function renderVncOptions(i) {
  const sel = document.getElementById('vnc-' + i);
  if (!sel) return;
  let html = '<option value="">本机 VNC 虚拟机：手动输入或从列表选择</option>';
  if (!VNC_HOSTS.length) {
    html = '<option value="">未检测到本机 VNC 虚拟机，请手动填写 VNC 地址</option>';
  } else {
    for (const h of VNC_HOSTS) {
      html += `<option value="${esc(h.path)}">${esc(h.name)}（本机虚拟机）</option>`;
    }
  }
  sel.innerHTML = html;
  syncVncSelect(i);
}

function syncVncSelect(i) {
  const sel = document.getElementById('vnc-' + i);
  const a = document.getElementById('arg-' + i);
  if (!sel || !a) return;
  const raw = a.value.trim();
  const path = /^VNC:/i.test(raw) ? stripProto(raw, 'vnc') : raw;
  let found = '';
  for (const opt of sel.options) {
    if (opt.value && opt.value === path) { found = path; break; }
  }
  sel.value = found;
}

function pickVnc(i) {
  const sel = document.getElementById('vnc-' + i);
  const a = document.getElementById('arg-' + i);
  if (!sel || !a || !sel.value) { if (a) a.focus(); return; }
  a.value = sel.value;
  DIRTY[i] = 1;
  PENDING[i] = null;
  LASTA[i] = a.value;
}

function typeLabel(type) {
  if (type === 'remote') return '远程连接';
  if (type === 'rdp') return '远程桌面（RDP，旧配置）';
  if (type === 'vnc') return '远程桌面（VNC，旧配置）';
  return type;
}

function build(s) {
  const types = s.types || [];
  VNC_HOSTS = s.vnc_hosts || [];
  VNC_SIG = JSON.stringify(VNC_HOSTS);
  const outs = (s.outputs || []).filter((o) => o.connected);
  outs.forEach((o) => {
    NAMES[o.index] = o.name;
    if (o.source_arg) remember(o.name, o.source_type, o.source_arg);
  });
  const box = $('outputs');
  if (!box) return;
  if (!outs.length) {
    box.innerHTML = `<div class="empty">未检测到已连接的显示器<span>接入 HDMI/DP 后会自动出现在这里，无需刷新页面</span></div>`;
    return;
  }
  box.innerHTML = outs.map((o) => {
    let typeOpts = types.map((t) =>
      `<option value="${esc(t.id)}"${t.id === o.source_type ? ' selected' : ''}>${esc(t.label)}</option>`).join('');
    if (!types.some((t) => t.id === o.source_type)) {
      typeOpts = `<option value="${esc(o.source_type)}" selected>${esc(typeLabel(o.source_type))}</option>` + typeOpts;
    }
    let modeOpts = `<option value="0x0">自动</option>`;
    (o.modes || []).forEach((m) => {
      const sel = (o.active && m.w === o.width && m.h === o.height) ? ' selected' : '';
      modeOpts += `<option value="${m.w}x${m.h}"${sel}>${m.w}×${m.h}@${m.r}</option>`;
    });
    const preview = o.active
      ? `<img class="preview" loading="lazy" src="${esc(api('preview/' + encodeURIComponent(o.name) + '.mjpg'))}" alt="">`
      : `<div class="nopreview">未启用</div>`;
    return `<section class="card${o.active ? ' on' : ''}" data-index="${o.index}">
      <div class="card-head">
        <b>#${o.index} ${esc(o.name)}</b>
        <span class="badge">${o.width}×${o.height}@${o.refresh}</span>
        <span class="state ${o.active ? 'on' : 'off'}">${o.active ? 'ON' : 'OFF'}</span>
      </div>
      ${preview}
      <div class="meta" id="meta-${o.index}"></div>
      <div class="set">
        <select id="type-${o.index}" onchange="onTypeChange(${o.index})">${typeOpts}</select>
        <input id="arg-${o.index}" class="grow" value="${esc(o.source_arg)}" placeholder="URL / 文件 / 地址" spellcheck="false" oninput="onArgInput(${o.index})">
      </div>
      <div class="set vnc-row" id="vncrow-${o.index}" style="display:none">
        <select id="vnc-${o.index}" onchange="pickVnc(${o.index})"></select>
      </div>
      <div class="set">
        <select id="res-${o.index}">${modeOpts}</select>
        <button type="button" class="primary" onclick="applySource(${o.index})">应用</button>
        <button type="button" class="danger-btn" onclick="disableSource(${o.index})">停用</button>
      </div>
    </section>`;
  }).join('');
  outs.forEach((o) => renderVncOptions(o.index));
  outs.forEach((o) => syncType(o.index));
  outs.forEach((o) => {
    LASTT[o.index] = o.source_type;
    LASTA[o.index] = o.source_arg;
    DIRTY[o.index] = 0;
    PENDING[o.index] = null;
  });
}

function update(s) {
  (s.outputs || []).forEach((o) => { NAMES[o.index] = o.name; });
  const hosts = s.vnc_hosts || [];
  const hs = JSON.stringify(hosts);
  if (hs !== VNC_SIG) {
    VNC_SIG = hs;
    VNC_HOSTS = hosts;
    (s.outputs || []).forEach((o) => {
      if (o.connected) renderVncOptions(o.index);
    });
  }
  (s.outputs || []).forEach((o) => {
    if (!o.connected) return;
    const m = document.getElementById('meta-' + o.index);
    if (m) {
      let t = (o.active ? 'ON' : 'OFF') + ' · ' + (o.codec || '无信号');
      t += ' · ' + (o.fps ? o.fps.toFixed(0) : '0') + 'fps';
      t += ' · 输出 ' + (o.out_fps ? o.out_fps.toFixed(1) : '0') + 'fps';
      if (o.error) t += ' · ' + o.error;
      if (o.open_error) t += ' · ' + o.open_error;
      m.textContent = t;
    }
    if (DIRTY[o.index]) {
      const p = PENDING[o.index];
      if (p && o.source_type === p.type && o.source_arg === p.arg) {
        DIRTY[o.index] = 0;
        PENDING[o.index] = null;
      } else {
        return;
      }
    }
    const arg = document.getElementById('arg-' + o.index);
    if (arg && LASTA[o.index] !== o.source_arg) {
      arg.value = o.source_arg;
      LASTA[o.index] = o.source_arg;
      if (o.source_arg) remember(o.name, o.source_type, o.source_arg);
    }
    const sel = document.getElementById('type-' + o.index);
    if (sel && LASTT[o.index] !== o.source_type) {
      sel.dataset.prev = o.source_type;
      sel.value = o.source_type;
      LASTT[o.index] = o.source_type;
      syncType(o.index);
      if (arg) {
        arg.value = o.source_arg;
        LASTA[o.index] = o.source_arg;
        remember(o.name, o.source_type, o.source_arg);
      }
    }
  });
}

async function applySource(i) {
  const typeEl = document.getElementById('type-' + i);
  const argEl = document.getElementById('arg-' + i);
  const resEl = document.getElementById('res-' + i);
  if (!typeEl || !argEl || !resEl) return;
  const type = typeEl.value;
  let arg = argEl.value;
  if (type === 'ui' || type === 'bars') arg = '';
  if (type === 'remote') {
    const raw = arg.trim();
    if (!raw) {
      toast('请输入远程地址：用户:密码@主机 或 VNC 主机:端口');
      return;
    }
  }
  if (type === 'rdp' && !/^.+:.+@.+/.test(stripProto(arg.trim(), 'rdp'))) {
    toast('RDP 格式：RDP:用户:密码@主机[:端口]');
    return;
  }
  if (type === 'vnc' && !stripProto(arg.trim(), 'vnc')) {
    toast('请输入 VNC 地址，例如 VNC:主机:端口');
    return;
  }
  const res = resEl.value.split('x');
  const url = api('api/apply?out=' + encodeURIComponent(NAMES[i] || '') +
    '&type=' + encodeURIComponent(type) +
    '&arg=' + encodeURIComponent(arg) +
    '&w=' + encodeURIComponent(res[0]) + '&h=' + encodeURIComponent(res[1]));
  $('sys').textContent = '正在应用 #' + i + ' …';
  try {
    const r = await fetch(url);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    DIRTY[i] = 1;
    PENDING[i] = { type: type, arg: arg };
    LASTA[i] = arg;
    remember(NAMES[i], type, arg);
    toast('已应用');
  } catch (e) {
    toast('应用失败：' + e.message);
  }
  setTimeout(refresh, 500);
}

async function disableSource(i) {
  try {
    const r = await fetch(api('api/disable?out=' + encodeURIComponent(NAMES[i] || '')));
    if (!r.ok) throw new Error('HTTP ' + r.status);
    DIRTY[i] = 0;
    PENDING[i] = null;
    toast('已停用');
  } catch (e) {
    toast('停用失败：' + e.message);
  }
  setTimeout(refresh, 500);
}

async function quit() {
  if (!confirm('停止 HDMI Display 服务？')) return;
  try { await fetch(api('api/quit')); } catch (e) {}
  $('sys').textContent = '已请求停止';
}

function typeInfo(kind) {
  const types = [
    [1, '键盘', '文字输入', '#60a5fa'],
    [2, '鼠标', '指针/点击', '#34d399'],
    [4, '触控板', '指针/手势', '#22d3ee'],
    [8, '触摸屏', '触控', '#38bdf8'],
    [16, '手写笔', '绘图/手写', '#a78bfa'],
    [32, '手柄', '游戏控制', '#fb923c'],
    [1024, '鼠标扩展键', '宏/媒体键', '#f59e0b'],
    [64, '媒体键', '音量/播放', '#f472b6'],
    [128, '系统键', '电源/睡眠', '#f87171'],
    [256, '遥控器', '媒体遥控', '#facc15'],
    [512, '其它', '原始输入', '#94a3b8'],
  ];
  for (const t of types) if (kind & t[0]) return { name: t[1], purpose: t[2], color: t[3] };
  return { name: '输入设备', purpose: '原始输入', color: '#94a3b8' };
}

function pickInput(role, nodes) {
  const src = nodes || INDEVS;
  const pool = src.filter((d) => role === 'kbd' ? (d.kind & 1) : (d.kind & 2));
  if (!pool.length) return null;
  const active = pool.filter((d) => (d.events || 0) > 0);
  if (active.length) {
    active.sort((a, b) => (b.last_ms || 0) - (a.last_ms || 0));
    return active[0];
  }
  let best = null, bs = -1e9;
  for (const d of pool) {
    const n = (d.name || '').toLowerCase();
    let s = 0;
    if (role === 'kbd') {
      if (n.includes('keyboard')) s += 10;
      if (n.includes('gaming mouse')) s -= 20;
      if (n.includes('consumer') || n.includes('system control')) s -= 8;
    } else {
      if (n.includes('mouse')) s += 10;
      if (n.includes('keyboard')) s -= 8;
      if (n.includes('gaming mouse')) s += 3;
    }
    if (d.output) s += 2;
    if (s > bs) { bs = s; best = d; }
  }
  return best;
}

function roleList(nodes) {
  let bits = 0;
  nodes.forEach((d) => { bits |= (d.kind || 0); });
  return [1, 2, 4, 8, 16, 32, 1024, 64, 128, 256, 512].filter((b) => bits & b);
}

function renderInputs(s) {
  LASTSTATE = s;
  const box = $('inputs');
  if (!box) return;
  if (!INDEVS.length) {
    box.innerHTML = '<div class="empty">未检测到输入设备<span>Linux 需要 /dev/input 读取权限</span></div>';
    return;
  }
  const outs = (s.outputs || []).filter(
    (o) => o.connected && o.active &&
      (BINDABLE.has(o.source_type) ||
       (o.source_type === 'pip' && o.input_ok)));
  if (!outs.length) {
    box.innerHTML = '<div class="empty">当前没有可绑定键鼠的屏幕<span>先在输出卡片选择“远程桌面 / 网页 / VNC”，或包含这些层的画中画</span></div>';
    INROWS = [];
    return;
  }
  const rows = [];
  if (SHOWALL) {
    const map = {}, order = [];
    INDEVS.forEach((d) => {
      const key = d.group || d.id;
      let g = map[key];
      if (!g) { g = { name: d.group || d.name, nodes: [], output: d.output || '' }; map[key] = g; order.push(g); }
      g.nodes.push(d);
      if (d.output) g.output = d.output;
    });
    order.forEach((g) => {
      const kbd = pickInput('kbd', g.nodes);
      const mouse = pickInput('mouse', g.nodes);
      const ids = [];
      if (kbd) ids.push(kbd.id);
      if (mouse && (!kbd || mouse.id !== kbd.id)) ids.push(mouse.id);
      g.nodes.forEach((d) => {
        if (d === kbd || d === mouse) return;
        if (d.kind & (4 | 8 | 16 | 32 | 1024 | 64 | 128 | 256 | 512)) ids.push(d.id);
      });
      if (!ids.length) return;
      rows.push({ ids, name: g.name, kinds: roleList(g.nodes), output: g.output });
    });
  } else {
    const k = pickInput('kbd');
    const m = pickInput('mouse');
    if (k) rows.push({ ids: [k.id], name: k.name, kinds: roleList([k]), output: k.output });
    if (m) rows.push({ ids: [m.id], name: m.name, kinds: roleList([m]), output: m.output });
  }
  INROWS = rows;
  const bar = `<div class="inputs-head"><span class="hint">${SHOWALL
    ? '按物理设备显示；每行列出键盘/鼠标/媒体键等实际用途'
    : '已自动选择键盘和鼠标，其余接口已隐藏'}</span><button class="ghost" onclick="toggleAll()">${SHOWALL ? '只看键盘和鼠标' : '显示全部设备'}</button></div>`;
  const list = rows.map((d, i) => {
    const opts = [`<option value="">不绑定</option>`].concat(outs.map((o) =>
      `<option value="${esc(o.name)}"${o.name === d.output ? ' selected' : ''}>${esc(o.name)}</option>`)).join('');
    const badges = d.kinds.map((k) => {
      const ti = typeInfo(k);
      return `<span class="type-badge" style="color:${ti.color};border-color:${ti.color}66">${ti.name}</span>`;
    }).join('');
    const purpose = d.kinds.map((k) => typeInfo(k).purpose).join(' / ');
    return `<div class="input-row">
      <div class="input-title">${badges}<span class="name" title="${esc(d.ids.join(' '))}">${esc(d.name)}</span><span class="purpose">· ${esc(purpose)}</span></div>
      <div class="input-bind"><select id="in${i}">${opts}</select><button type="button" class="ghost" onclick="bindInput(${i})">绑定</button></div>
    </div>`;
  }).join('');
  box.innerHTML = bar + list;
}

function toggleAll() {
  SHOWALL = !SHOWALL;
  if (LASTSTATE) renderInputs(LASTSTATE);
}

async function bindInput(i) {
  const d = INROWS[i];
  if (!d) return;
  const sel = document.getElementById('in' + i);
  const out = sel ? sel.value : '';
  try {
    for (const id of d.ids) {
      const r = await fetch(api('api/input/bind?id=' + encodeURIComponent(id) +
        '&out=' + encodeURIComponent(out)));
      if (!r.ok) throw new Error('HTTP ' + r.status);
    }
    toast(out ? '已绑定到 ' + out : '已取消绑定');
  } catch (e) {
    toast('绑定失败：' + e.message);
  }
  refresh();
}

async function refresh() {
  try {
    const r = await fetch(noCache(api('api/state')));
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const s = await r.json();
    const sys = $('sys');
    if (sys) sys.textContent = s.host + ' · 运行 ' + fmtUp(s.uptime) + ' · 内存 ' + s.mem_free + '/' + s.mem_total + ' MB';
    if ($('version') && s.version) $('version').textContent = 'v' + s.version;
    const nsig = (s.outputs || []).map((o) =>
      o.name + ':' + (o.active ? 1 : 0) + ':' + (o.connected ? 1 : 0) + ':' +
      o.source_type + ':' + o.width + 'x' + o.height).join('|');
    if (nsig !== sig) { build(s); sig = nsig; }
    update(s);
    INDEVS = s.inputs || [];
    const chosen = ((pickInput('kbd') || {}).id || '') + ',' + ((pickInput('mouse') || {}).id || '');
    const isig = INDEVS.map((d) =>
      (d.group || d.id) + ':' + d.output + ':' + d.kind + ':' + ((d.events || 0) > 0 ? 1 : 0)).join('|') +
      '#' + chosen + '#' +
      (s.outputs || []).map((o) =>
        o.name + ':' + o.source_type + ':' + (o.input_ok ? 1 : 0)).join(',');
    if (isig !== IN_SIG) { IN_SIG = isig; renderInputs(s); }
  } catch (e) {
    const sys = $('sys');
    if (sys) sys.textContent = '连接断开，正在重试…';
  }
}

function start() {
  const dl = $('dl-rdp');
  if (dl) dl.href = api('rdp/enable-rdp.ps1');
  const rb = $('refreshBtn');
  if (rb) rb.addEventListener('click', refresh);
  const qb = $('quitBtn');
  if (qb) qb.addEventListener('click', quit);
  refresh();
  refreshTimer = setInterval(refresh, 1000);
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', start);
} else {
  start();
}
