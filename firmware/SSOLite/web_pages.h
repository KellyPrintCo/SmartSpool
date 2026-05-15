/*
 * web_pages.h - HTML / CSS / JS for the embedded SSO-Lite web UI.
 * Stored in PROGMEM to keep RAM free.
 */
#pragma once
#include <Arduino.h>

const char PAGE_INDEX[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmartSpool OS Lite</title>
<link rel="stylesheet" href="/style.css">
</head>
<body>
<header>
  <h1>SmartSpool OS Lite</h1>
  <nav>
    <button data-tab="dash" class="active">Dashboard</button>
    <button data-tab="tag">Tag Manager</button>
    <button data-tab="profiles">Profiles</button>
    <button data-tab="ctrl">Controls</button>
    <button data-tab="setup">Setup</button>
  </nav>
</header>

<main>
  <!-- Dashboard ------------------------------------------------------- -->
  <section id="dash" class="tab active">
    <div class="grid">
      <div class="card">
        <h2>AMS Slot 1</h2>
        <div id="slotSwatch" class="swatch big"></div>
        <div id="slotInfo">--</div>
      </div>
      <div class="card">
        <h2>Last Scan</h2>
        <div id="lastSwatch" class="swatch big"></div>
        <div id="lastInfo">--</div>
      </div>
      <div class="card">
        <h2>Connection</h2>
        <div class="row"><span>Wi-Fi</span><span id="wifi" class="pill">--</span></div>
        <div class="row"><span>MQTT</span><span id="mqtt" class="pill">--</span></div>
        <div class="row"><span>Auto-assign</span><span id="auto" class="pill">--</span></div>
        <div class="row"><span>Auto-rewrite</span><span id="autorw" class="pill">--</span></div>
        <div class="row"><span>Official tag lock</span><span id="lock" class="pill">--</span></div>
        <div class="row"><span>Firmware</span><span id="fw">--</span></div>
      </div>
    </div>
    <div id="rewriteCard" class="card rewrite-pending" hidden>
      <h2>Pending tag rewrite</h2>
      <div class="row">
        <div>
          <span id="rewriteSwatch" class="swatch" style="vertical-align:middle"></span>
          <b id="rewriteCode">--</b>
          <small id="rewriteUid" style="color:var(--mut);margin-left:8px"></small>
        </div>
        <button id="btnCancelRewrite" class="ghost" type="button">Cancel</button>
      </div>
      <p class="muted" style="margin:6px 0 0;font-size:.9em">
        You changed slot 1 on the touchscreen. Tap the matching tag on the
        reader and SmartSpool will write the new info to it.
      </p>
    </div>
    <div class="card">
      <h2>Recent Tags</h2>
      <div id="history" class="history"></div>
    </div>
  </section>

  <!-- Tag Manager ----------------------------------------------------- -->
  <section id="tag" class="tab">
    <div class="card">
      <h2>Write a Tag</h2>
      <p>Pick a profile and present the tag to the reader within 60 seconds.</p>
      <select id="writeSel"></select>
      <button id="btnWrite">Queue Write</button>
      <button id="btnCancelWrite" class="ghost">Cancel</button>
      <div id="writeMsg" class="msg"></div>
    </div>
    <div class="card">
      <h2>Quick Write Presets</h2>
      <div id="presets" class="presets"></div>
    </div>
  </section>

  <!-- Profiles -------------------------------------------------------- -->
  <section id="profiles" class="tab">
    <div class="card">
      <h2>Filament Profiles</h2>
      <div id="profileList"></div>
    </div>
    <div class="card">
      <h2>Add / Edit Profile</h2>
      <form id="profileForm" onsubmit="return false;">
        <label>Name <input name="name" required></label>
        <label>Brand <input name="brand" value="Generic"></label>
        <label>Material
          <select name="material">
            <option>PLA</option><option>PETG</option><option>ABS</option>
            <option>TPU</option><option>PA</option><option>PC</option><option>PVA</option>
          </select>
        </label>
        <label>Bambu Code
          <select name="code">
            <option value="GFL99">Generic PLA (GFL99)</option>
            <option value="GFG99">Generic PETG (GFG99)</option>
            <option value="GFB99">Generic ABS (GFB99)</option>
            <option value="GFU99">Generic TPU (GFU99)</option>
            <option value="GFN99">Generic PA (GFN99)</option>
            <option value="GFC99">Generic PC (GFC99)</option>
            <option value="GFS99">Generic PVA (GFS99)</option>
          </select>
        </label>
        <label>Color <input type="color" name="color" value="#000000"></label>
        <label>Nozzle min <input name="nozzle_min" type="number" value="200"></label>
        <label>Nozzle max <input name="nozzle_max" type="number" value="230"></label>
        <label>Bed temp  <input name="bed"        type="number" value="60"></label>
        <button id="btnSaveProfile">Save Profile</button>
        <span id="profileMsg" class="msg"></span>
      </form>
    </div>
  </section>

  <!-- Controls -------------------------------------------------------- -->
  <section id="ctrl" class="tab">
    <div class="card">
      <h2>Force Assign</h2>
      <select id="assignSel"></select>
      <button id="btnAssign">Send to Printer</button>
      <div id="assignMsg" class="msg"></div>
    </div>
    <div class="card">
      <h2>System</h2>
      <button id="btnToggleAuto">Toggle Auto-Assign</button>
      <button id="btnToggleRewrite">Toggle Auto-Rewrite</button>
      <button id="btnReboot" class="danger">Reboot Device</button>
      <p class="muted" style="margin-top:8px;font-size:.85em">
        <b>Auto-rewrite</b> updates the RFID tag whenever you change slot 1
        info on the printer touchscreen. Off by default.
      </p>
    </div>
    <div class="card">
      <h2>Firmware Update</h2>
      <div class="row"><span>Current version</span><span id="fwVer" class="mono">--</span></div>
      <div class="row"><span>Latest version</span><span id="fwLatest" class="mono">--</span></div>
      <div id="fwUpdateRow" style="margin-top:10px;display:none">
        <div id="fwNotes" class="muted" style="font-size:.85em;margin-bottom:8px"></div>
        <div id="fwBar" class="ota-bar" style="display:none">
          <div id="fwBarFill" class="ota-bar-fill"></div>
        </div>
        <div id="fwStatus" class="msg"></div>
        <button id="btnFwUpdate" class="primary" style="margin-top:6px">Install Update</button>
      </div>
      <button id="btnFwCheck" class="ghost" style="margin-top:8px">Check for Updates</button>
    </div>
  </section>

  <!-- Setup ----------------------------------------------------------- -->
  <section id="setup" class="tab">
    <div class="card">
      <h2>Network &amp; Printer</h2>
      <form id="cfgForm" onsubmit="return false;">
        <label>Wi-Fi SSID  <input name="wifi_ssid"></label>
        <label>Wi-Fi Password <input name="wifi_pass" type="password"></label>
        <label>Printer IP <input name="printer_ip" placeholder="192.168.1.50"></label>
        <label>Printer Serial <input name="printer_serial" placeholder="01P00A123456789"></label>
        <label>LAN Access Code <input name="lan_code" type="password" placeholder="from printer screen"></label>
        <label class="checkbox"><input type="checkbox" name="auto_assign"> Auto-assign on scan</label>
        <label class="checkbox"><input type="checkbox" name="auto_rewrite"> Auto-rewrite tag on touchscreen change <small style="color:var(--mut)">(off by default)</small></label>
        <button id="btnSaveCfg">Save</button>
        <span id="cfgMsg" class="msg"></span>
      </form>
    </div>
  </section>
</main>
<script src="/app.js"></script>
</body>
</html>
)HTML";

const char PAGE_CSS[] PROGMEM = R"CSS(
:root{--bg:#101418;--fg:#e6edf3;--mut:#7d8590;--card:#161b22;--brd:#30363d;
      --acc:#2f81f7;--ok:#3fb950;--err:#f85149;--warn:#d29922;}
*{box-sizing:border-box}html,body{margin:0;font-family:system-ui,-apple-system,sans-serif;
  background:var(--bg);color:var(--fg);line-height:1.45}
header{padding:14px 18px;background:#0d1117;border-bottom:1px solid var(--brd);
  position:sticky;top:0;z-index:10}
header h1{margin:0 0 8px;font-size:1.3em}
nav button{background:transparent;color:var(--mut);border:0;padding:6px 10px;cursor:pointer;
  font-size:.95em;border-bottom:2px solid transparent;margin-right:4px}
nav button.active{color:var(--fg);border-color:var(--acc)}
main{padding:14px;max-width:980px;margin:0 auto}
.tab{display:none}.tab.active{display:block}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px;margin-bottom:14px}
.card{background:var(--card);border:1px solid var(--brd);border-radius:8px;padding:14px;margin-bottom:14px}
.card h2{margin:0 0 10px;font-size:1.05em;color:var(--mut);text-transform:uppercase;letter-spacing:.5px}
.swatch{width:32px;height:32px;border-radius:6px;border:1px solid var(--brd);display:inline-block;vertical-align:middle}
.swatch.big{width:54px;height:54px;margin-bottom:8px;display:block}
.row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #20262d}
.row:last-child{border:0}
.pill{background:#0d1117;border:1px solid var(--brd);border-radius:999px;padding:1px 10px;font-size:.85em}
.pill.ok{color:var(--ok);border-color:var(--ok)}.pill.err{color:var(--err);border-color:var(--err)}
.pill.warn{color:var(--warn);border-color:var(--warn)}
button{background:var(--acc);color:#fff;border:0;border-radius:6px;padding:8px 14px;
  cursor:pointer;font-size:.95em;margin:4px 6px 4px 0}
button.ghost{background:transparent;border:1px solid var(--brd);color:var(--fg)}
button.danger{background:var(--err)}
input,select{background:#0d1117;color:var(--fg);border:1px solid var(--brd);border-radius:6px;
  padding:7px 9px;font-size:.95em;margin:2px 0 8px;width:100%}
label{display:block;font-size:.9em;color:var(--mut);margin-top:6px}
label.checkbox{display:flex;gap:8px;align-items:center}
label.checkbox input{width:auto}
.msg{display:inline-block;margin-left:8px;color:var(--mut);font-size:.9em}
.msg.ok{color:var(--ok)}.msg.err{color:var(--err)}
.history .h-item{display:flex;align-items:center;gap:10px;padding:6px 0;border-bottom:1px solid #20262d}
.history .h-item:last-child{border:0}
.card.rewrite-pending{
  border-color:var(--warn);
  box-shadow:0 0 0 1px rgba(210,153,34,.2);
}
.card.rewrite-pending h2{color:var(--warn)}
.ota-bar{height:8px;background:#0d1117;border-radius:999px;overflow:hidden;margin:8px 0}
.ota-bar-fill{height:100%;width:0%;background:linear-gradient(90deg,var(--acc),#22a06b);
  transition:width .4s ease;border-radius:999px}
.mono{font-family:ui-monospace,Consolas,monospace;font-size:.85em;color:var(--mut)}
.profile-row{display:flex;align-items:center;gap:10px;padding:6px 0;border-bottom:1px solid #20262d}
.profile-row .meta{flex:1}
.profile-row small{color:var(--mut)}
.presets{display:flex;flex-wrap:wrap;gap:6px}
.presets button{font-size:.85em;padding:6px 10px}
form{display:grid;grid-template-columns:1fr 1fr;gap:8px}
form button,form .msg,form label.checkbox{grid-column:1 / -1}
@media(max-width:520px){form{grid-template-columns:1fr}}
)CSS";

const char PAGE_JS[] PROGMEM = R"JS(
const $ = s => document.querySelector(s);
const $$ = s => document.querySelectorAll(s);

/* tab routing */
$$('nav button').forEach(b=>b.onclick=()=>{
  $$('nav button').forEach(x=>x.classList.remove('active'));
  $$('.tab').forEach(x=>x.classList.remove('active'));
  b.classList.add('active');
  $('#'+b.dataset.tab).classList.add('active');
});

/* dashboard polling */
async function refresh(){
  try{
    const s = await (await fetch('/api/status')).json();
    $('#fw').textContent  = s.fw;
    $('#wifi').textContent= s.wifi_ok?('OK '+s.wifi_ip+' ('+s.wifi_rssi+'dBm)'):'down';
    $('#wifi').className  = 'pill '+(s.wifi_ok?'ok':'err');
    $('#mqtt').textContent= s.mqtt_ok?'connected':'down';
    $('#mqtt').className  = 'pill '+(s.mqtt_ok?'ok':'err');
    $('#auto').textContent= s.auto?'enabled':'disabled';
    $('#auto').className  = 'pill '+(s.auto?'ok':'warn');
    $('#autorw').textContent = s.auto_rewrite?'enabled':'disabled';
    $('#autorw').className   = 'pill '+(s.auto_rewrite?'ok':'warn');
    $('#lock').textContent= s.official_tag_lock?'engaged':'inactive';
    $('#lock').className  = 'pill '+(s.official_tag_lock?'warn':'ok');

    /* Pending rewrite banner */
    const pr = s.pending_rewrite;
    if (pr && pr.pending) {
      $('#rewriteCard').hidden = false;
      $('#rewriteSwatch').style.background = '#'+(pr.color||'808080');
      $('#rewriteCode').textContent = (pr.material||'?')+' / '+(pr.code||'?');
      $('#rewriteUid').textContent  = 'UID '+pr.uid;
    } else {
      $('#rewriteCard').hidden = true;
    }

    $('#slotSwatch').style.background = '#'+(s.slot1.color||'808080');
    $('#slotInfo').textContent = (s.slot1.material||'?')+' / '+(s.slot1.code||'?');

    if (s.last_scan && s.last_scan.valid){
      $('#lastSwatch').style.background = '#'+s.last_scan.color;
      $('#lastInfo').innerHTML = `<b>${s.last_scan.name}</b><br>
        <small>${s.last_scan.material} / ${s.last_scan.code} · UID ${s.last_scan.uid}<br>
        source: ${s.last_scan.source} · sent: ${s.last_scan.sent?'yes':'no'}</small>`;
    } else {
      $('#lastInfo').textContent = '(no scans yet)';
    }

    const h = $('#history'); h.innerHTML='';
    (s.history||[]).forEach(x=>{
      const d = document.createElement('div');
      d.className='h-item';
      d.innerHTML = `<span class="swatch" style="background:#${x.color}"></span>
                     <div><b>${x.name}</b><br><small>${x.code} · UID ${x.uid}</small></div>`;
      h.appendChild(d);
    });
  }catch(e){ console.warn(e); }
}
setInterval(refresh, 1500); refresh();

/* profiles */
async function loadProfiles(){
  const ps = await (await fetch('/api/profiles')).json();
  const list = $('#profileList'); list.innerHTML='';
  const sel1 = $('#writeSel');  sel1.innerHTML='';
  const sel2 = $('#assignSel'); sel2.innerHTML='';
  const presets = $('#presets'); presets.innerHTML='';
  ps.forEach(p=>{
    const row = document.createElement('div');
    row.className='profile-row';
    row.innerHTML = `<span class="swatch" style="background:#${p.color}"></span>
      <div class="meta"><b>${p.name}</b><br>
        <small>${p.brand} · ${p.material} · ${p.code} · N:${p.nozzle_min}-${p.nozzle_max} · B:${p.bed}</small>
      </div>
      <button class="ghost" data-edit='${JSON.stringify(p)}'>Edit</button>
      <button class="danger" data-del="${p.name}">Delete</button>`;
    list.appendChild(row);
    sel1.add(new Option(p.name, p.name));
    sel2.add(new Option(p.name, p.name));
    const b = document.createElement('button');
    b.textContent = p.name;
    b.style.borderLeft = '6px solid #'+p.color;
    b.onclick = ()=>{ $('#writeSel').value = p.name; queueWrite(); };
    presets.appendChild(b);
  });
  list.querySelectorAll('[data-del]').forEach(b=>b.onclick=async()=>{
    if(!confirm('Delete '+b.dataset.del+'?'))return;
    await fetch('/api/profiles?name='+encodeURIComponent(b.dataset.del),{method:'DELETE'});
    loadProfiles();
  });
  list.querySelectorAll('[data-edit]').forEach(b=>b.onclick=()=>{
    const p = JSON.parse(b.dataset.edit);
    const f = $('#profileForm');
    f.name.value = p.name; f.brand.value=p.brand; f.material.value=p.material;
    f.code.value=p.code; f.color.value='#'+p.color;
    f.nozzle_min.value=p.nozzle_min; f.nozzle_max.value=p.nozzle_max; f.bed.value=p.bed;
    location.hash='profiles';
  });
}
loadProfiles();

$('#btnSaveProfile').onclick = async ()=>{
  const f = $('#profileForm');
  const body = {
    name: f.name.value, brand: f.brand.value, material: f.material.value,
    code: f.code.value, color: f.color.value.replace('#',''),
    nozzle_min: +f.nozzle_min.value, nozzle_max: +f.nozzle_max.value, bed: +f.bed.value
  };
  if(!body.name){ $('#profileMsg').textContent='name required'; return; }
  const r = await fetch('/api/profiles',{method:'POST',body:JSON.stringify(body)});
  $('#profileMsg').textContent = r.ok?'saved':'failed';
  $('#profileMsg').className = 'msg '+(r.ok?'ok':'err');
  loadProfiles();
};

async function queueWrite(){
  const name = $('#writeSel').value;
  if(!name) return;
  const r = await fetch('/api/write',{method:'POST',body:JSON.stringify({name})});
  $('#writeMsg').textContent = await r.text();
  $('#writeMsg').className   = 'msg '+(r.ok?'ok':'err');
}
$('#btnWrite').onclick = queueWrite;
$('#btnCancelWrite').onclick = async ()=>{
  const r = await fetch('/api/write/cancel',{method:'POST'});
  $('#writeMsg').textContent = await r.text();
  $('#writeMsg').className   = 'msg';
};

$('#btnAssign').onclick = async ()=>{
  const name = $('#assignSel').value;
  const r = await fetch('/api/assign',{method:'POST',body:JSON.stringify({name})});
  $('#assignMsg').textContent = await r.text();
  $('#assignMsg').className   = 'msg '+(r.ok?'ok':'err');
};
$('#btnToggleAuto').onclick = async ()=>{
  await fetch('/api/auto',{method:'POST'}); refresh();
};
$('#btnToggleRewrite').onclick = async ()=>{
  await fetch('/api/rewrite',{method:'POST'}); refresh();
};
$('#btnCancelRewrite').onclick = async ()=>{
  await fetch('/api/rewrite/cancel',{method:'POST'}); refresh();
};
$('#btnReboot').onclick = async ()=>{
  if(!confirm('Reboot device?')) return;
  await fetch('/api/reboot',{method:'POST'});
};

/* setup form */
async function loadCfg(){
  const c = await (await fetch('/api/config')).json();
  const f = $('#cfgForm');
  f.wifi_ssid.value      = c.wifi_ssid||'';
  f.printer_ip.value     = c.printer_ip||'';
  f.printer_serial.value = c.printer_serial||'';
  f.auto_assign.checked  = !!c.auto_assign;
  f.auto_rewrite.checked = !!c.auto_rewrite;
  if (c.lan_code_set) f.lan_code.placeholder = '(set; leave blank to keep)';
}
loadCfg();
$('#btnSaveCfg').onclick = async ()=>{
  const f = $('#cfgForm');
  const body = {
    wifi_ssid: f.wifi_ssid.value, printer_ip: f.printer_ip.value,
    printer_serial: f.printer_serial.value,
    auto_assign:  f.auto_assign.checked,
    auto_rewrite: f.auto_rewrite.checked
  };
  if (f.wifi_pass.value) body.wifi_pass = f.wifi_pass.value;
  if (f.lan_code.value)  body.lan_code  = f.lan_code.value;
  const r = await fetch('/api/config',{method:'POST',body:JSON.stringify(body)});
  $('#cfgMsg').textContent = await r.text();
  $('#cfgMsg').className   = 'msg '+(r.ok?'ok':'err');
};

/* ---- Firmware update ---- */
/* In v1.3.3+ the dashboard hits the ESP32's /api/check-update which proxies
 * to raw.githubusercontent.com. This avoids Chrome's mixed-content / Private
 * Network Access blocking that prevents an http://ssolite.local page from
 * fetching https URLs on local networks. */
const VERSION_JSON = '/api/check-update';
let _latestFw = null;

/* Always populate current FW from the dashboard pill on click; safer than reading at script load. */
$('#btnFwCheck').onclick = checkFw;

async function checkFw() {
  /* Update the "Current version" cell from /api/health (fresh, not cached). */
  $('#fwVer').textContent = 'checking…';
  try {
    const h = await fetch('/api/health?t=' + Date.now(), { cache: 'no-store' });
    const hd = await h.json();
    $('#fwVer').textContent = hd.fw || '--';
  } catch (e) {
    $('#fwVer').textContent = '?';
  }

  $('#fwLatest').textContent = 'checking…';
  $('#fwStatus').textContent = '';
  $('#fwStatus').className   = 'msg';
  let r;
  try {
    /* Cache busting: query string + cache:no-store. We intentionally
     * avoid setting a Cache-Control request header here, because doing so
     * forces a CORS preflight (OPTIONS) which GitHub's raw.githubusercontent.com
     * does not always answer with the matching Allow-Headers. The query
     * string + cache:no-store is enough to bypass the browser cache. */
    r = await fetch(VERSION_JSON + '?t=' + Date.now() + '&nocache=' + Math.random(),
                    { cache: 'no-store' });
  } catch (e) {
    $('#fwLatest').textContent = 'unreachable';
    $('#fwStatus').innerHTML =
      'Could not reach the update server.<br>' +
      '<small>' + (e.message || String(e)) + '</small><br>' +
      'Try opening <code>' + VERSION_JSON + '</code> in a new tab — if it loads, ' +
      'the issue is in this dashboard. If it 404s, version.json isn\'t at that URL on GitHub.';
    $('#fwStatus').className   = 'msg err';
    return;
  }
  if (!r.ok) {
    $('#fwLatest').textContent = 'not found';
    if (r.status === 404) {
      $('#fwStatus').innerHTML =
        'Update manifest not found on GitHub. The repo may not exist or version.json is not at the root.<br>' +
        'Tried: <code>' + VERSION_JSON + '</code>';
    } else {
      $('#fwStatus').textContent = 'Update server returned HTTP ' + r.status + '.';
    }
    $('#fwStatus').className   = 'msg err';
    return;
  }
  try {
    _latestFw = await r.json();
  } catch (e) {
    $('#fwLatest').textContent = 'bad data';
    $('#fwStatus').textContent =
      'Update manifest returned invalid JSON. Check that version.json is valid.';
    $('#fwStatus').className   = 'msg err';
    return;
  }
  $('#fwLatest').textContent = _latestFw.version;
  const cur = ($('#fwVer').textContent || '').replace(/^v/, '').trim();
  const newer = isNewer(_latestFw.version, cur);
  if (newer) {
    $('#fwUpdateRow').style.display = '';
    $('#fwNotes').textContent = _latestFw.release_notes || '';
    $('#fwStatus').textContent = 'Update available.';
    $('#fwStatus').className   = 'msg';
  } else if (cur === _latestFw.version) {
    $('#fwUpdateRow').style.display = 'none';
    $('#fwStatus').textContent = 'Up to date.';
    $('#fwStatus').className   = 'msg ok';
  } else {
    $('#fwUpdateRow').style.display = 'none';
    $('#fwStatus').textContent =
      'Installed version (' + cur + ') is newer than the published version (' + _latestFw.version + '). ' +
      'If you just pushed an update to GitHub, the CDN may be serving a cached copy — wait a minute and try again.';
    $('#fwStatus').className   = 'msg warn';
  }
}

function isNewer(latest, cur) {
  const p = s => s.split('.').map(Number);
  const [la,lb,lc] = p(latest); const [ca,cb,cc] = p(cur || '0.0.0');
  return la>ca || (la===ca && lb>cb) || (la===ca && lb===cb && lc>cc);
}

$('#btnFwUpdate').onclick = async ()=>{
  if (!_latestFw) return;
  const btn = $('#btnFwUpdate');
  btn.disabled = true;
  const bar = $('#fwBar'); bar.style.display = '';
  const fill = $('#fwBarFill');
  const stat = $('#fwStatus');
  const setProgress = (pct, msg, cls='') => {
    fill.style.width = pct + '%';
    stat.textContent = msg;
    stat.className   = 'msg ' + cls;
  };

  try {
    setProgress(5, 'Downloading firmware…');
    const binRes = await fetch(_latestFw.firmware_url);
    if (!binRes.ok) throw new Error('Download failed: ' + binRes.status);
    const blob = await binRes.blob();
    setProgress(30, 'Uploading to SmartSpool…');

    const fd = new FormData();
    fd.append('firmware', blob, 'firmware.bin');

    /* Simulate progress during the upload (we can't read upload progress
     * natively from fetch, so we animate on a 200ms timer) */
    let pct = 30;
    const tick = setInterval(()=>{ pct=Math.min(pct+2, 82); fill.style.width=pct+'%'; }, 200);

    const upRes = await fetch('/api/update', { method:'POST', body:fd });
    clearInterval(tick);

    if (!upRes.ok) {
      const err = await upRes.json().catch(()=>({}));
      throw new Error(err.error || 'Upload failed: ' + upRes.status);
    }

    setProgress(90, 'Flashing complete — rebooting SmartSpool…');
    await new Promise(r=>setTimeout(r, 12000));   // wait for reboot
    setProgress(98, 'Reconnecting…');

    /* Poll until the device is back online with the new version */
    for (let i=0; i<20; i++) {
      await new Promise(r=>setTimeout(r, 2000));
      try {
        const hRes = await fetch('/api/health');
        if (hRes.ok) {
          const h = await hRes.json();
          setProgress(100, 'Updated to v' + h.fw + ' ✓', 'ok');
          $('#fwVer').textContent = h.fw;
          break;
        }
      } catch { /* still rebooting */ }
      if (i === 19) throw new Error('Device did not come back online. Try refreshing.');
    }
  } catch(e) {
    setProgress(0, e.message, 'err');
    bar.style.display = 'none';
  }
  btn.disabled = false;
};
)JS";
