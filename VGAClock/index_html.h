// Web UI page (kept out of the .ino so the Arduino prototype generator does not parse the JavaScript)
#pragma once
#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>VGA Clock</title>
<style>
body{font-family:system-ui,sans-serif;background:#111;color:#ddd;margin:0 auto;padding:1em;max-width:640px}
h2{color:#fa0;margin:.2em 0}fieldset{border:1px solid #444;border-radius:6px;margin:0 0 1em}legend{color:#fa0}
label{display:block;margin:.45em 0}
input[type=text],input[type=password],input[type=number],select,textarea{width:100%;box-sizing:border-box;background:#222;color:#eee;border:1px solid #555;padding:.35em;border-radius:3px}
textarea{height:7em;font:11px monospace}button{background:#fa0;color:#000;border:0;padding:.5em 1em;border-radius:4px;margin:.2em 0;cursor:pointer}
.row{display:flex;gap:.6em}.row>*{flex:1}small{color:#888}
#st{font:12px monospace;white-space:pre;color:#8c8;background:#1a1a1a;padding:.5em;border-radius:4px;margin-bottom:1em}
</style></head><body>
<h2>VGA Clock</h2><div id="st">…</div>
<form id="f">
<fieldset><legend>Display</legend>
<label>Clock type <select name="mode" id="mode"><option value="0">Digital (14-segment)</option><option value="1">Analogue (station clock)</option></select></label>
<label>Resolution <select name="res" id="res"></select></label>
<label>Monitor shape <select name="aspect" id="aspect"></select></label>
<small>Shape of the picture as your monitor shows it. Auto = 4:3 for classic modes, square pixels for widescreen modes. Pick 16:9 / 16:10 if a widescreen LCD stretches a 4:3 mode to full width.</small>
<div id="dig">
<label>Digit colour <input type="color" id="color" name="color"> <small id="q_color"></small></label>
<label><input type="checkbox" name="ghost" id="ghost"> Show unlit segments (dim)</label>
<label><input type="checkbox" name="h24" id="h24"> 24-hour clock</label>
<label><input type="checkbox" name="blink" id="blink"> Blinking colon</label>
</div>
<div id="ana">
<div class="row">
<label>Dial <input type="color" id="face" name="face"> <small id="q_face"></small></label>
<label>Hands <input type="color" id="hand" name="hand"> <small id="q_hand"></small></label>
<label>Second hand <input type="color" id="sec" name="sec"> <small id="q_sec"></small></label>
</div>
<label><input type="checkbox" name="jump" id="jump"> Minute hand jumps every minute</label>
</div>
<label><input type="checkbox" name="secs" id="secs"> <span id="secl">Show seconds</span></label>
<small>The VGA DAC has 4 levels per channel (64 colours); picked colours are snapped to the nearest one.</small>
</fieldset>
<fieldset><legend>Time</legend>
<label>Time zone <select id="tzsel"></select></label>
<label>POSIX TZ string <input type="text" name="tz" id="tz"></label>
<label><input type="checkbox" name="ntp" id="ntp"> Use NTP</label>
<div class="row"><label>NTP server 1<input type="text" name="ntp1" id="ntp1"></label><label>NTP server 2<input type="text" name="ntp2" id="ntp2"></label></div>
<button type="button" id="setnow">Set clock from this browser</button>
</fieldset>
<fieldset><legend>RTC module (optional)</legend>
<label><input type="checkbox" name="rtc" id="rtc"> RTC module fitted</label>
<label>Type <select name="rtct" id="rtct"><option value="0">DS3231</option><option value="1">DS1307</option></select></label>
<div class="row"><label>SDA GPIO<input type="number" name="sda" id="sda"></label><label>SCL GPIO<input type="number" name="scl" id="scl"></label></div>
<small>Allowed: 2, 13, 14, 16, 17, 25, 26, 27, 32, 33 (VGA uses 4, 5, 15, 18, 19, 21, 22, 23)</small>
</fieldset>
<fieldset><legend>Wi-Fi</legend>
<label>Security <select name="ent" id="ent"><option value="0">WPA/WPA2-Personal</option><option value="1">WPA2-Enterprise (PEAP / TTLS)</option></select></label>
<label>SSID <input type="text" name="ssid" id="ssid" list="nets"><datalist id="nets"></datalist></label>
<button type="button" id="scan">Scan</button>
<div id="pskb"><label>Password <input type="password" name="psk" placeholder="(unchanged)"></label></div>
<div id="eapb">
<label>Outer / anonymous identity <input type="text" name="eapid" id="eapid" placeholder="e.g. anonymous@example.org"></label>
<label>Username <input type="text" name="eapuser" id="eapuser"></label>
<label>Password <input type="password" name="eappass" placeholder="(unchanged)"></label>
<label>CA certificate, PEM (recommended) <textarea name="eapca" id="eapca"></textarea></label>
</div>
</fieldset>
<fieldset><legend>System</legend>
<label>Hostname <input type="text" name="host" id="host"></label>
<label>Admin password (web UI + OTA) <input type="password" name="admin" placeholder="(unchanged)"></label>
</fieldset>
<button type="submit">Save</button> <span id="msg"></span>
</form>
<fieldset><legend>Firmware</legend>
<input type="file" id="fw" accept=".bin"> <button id="up">Upload</button> <progress id="pg" max="100" value="0"></progress><br>
<button id="rb">Reboot</button>
</fieldset>
<script>
const $=i=>document.getElementById(i);
const esc=s=>String(s).replace(/[&<>"']/g,c=>'&#'+c.charCodeAt(0)+';');
const TZ=[["UTC","UTC0"],["Europe/Amsterdam, Berlin, Paris","CET-1CEST,M3.5.0,M10.5.0/3"],["Europe/London, Dublin, Lisbon","GMT0BST,M3.5.0/1,M10.5.0"],
["Europe/Helsinki, Athens","EET-2EEST,M3.5.0/3,M10.5.0/4"],["Europe/Moscow","MSK-3"],["America/New_York","EST5EDT,M3.2.0,M11.1.0"],
["America/Chicago","CST6CDT,M3.2.0,M11.1.0"],["America/Denver","MST7MDT,M3.2.0,M11.1.0"],["America/Phoenix","MST7"],
["America/Los_Angeles","PST8PDT,M3.2.0,M11.1.0"],["America/Sao_Paulo","<-03>3"],["Asia/Dubai","<+04>-4"],["Asia/Kolkata","IST-5:30"],
["Asia/Shanghai","CST-8"],["Asia/Tokyo","JST-9"],["Australia/Sydney","AEST-10AEDT,M10.1.0,M4.1.0/3"],["Pacific/Auckland","NZST-12NZDT,M9.5.0,M4.1.0/3"]];
const COLS=['color','face','hand','sec'];
function q(h){const v=parseInt(h.slice(1),16);return '#'+[v>>16,v>>8&255,v&255].map(x=>(Math.round(x/85)*85).toString(16).padStart(2,'0')).join('')}
function showQ(k){$('q_'+k).textContent='→ '+q($(k).value)}
function vis(){const e=$('ent').value=='1';$('eapb').style.display=e?'':'none';$('pskb').style.display=e?'none':'';
 const a=$('mode').value=='1';$('dig').style.display=a?'none':'';$('ana').style.display=a?'':'none';$('secl').textContent=a?'Second hand':'Show seconds'}
function tzSync(){const o=[...$('tzsel').options].find(o=>o.value&&o.value==$('tz').value);$('tzsel').value=o?o.value:''}
$('tzsel').innerHTML=TZ.map(t=>`<option value="${esc(t[1])}">${esc(t[0])}</option>`).join('')+'<option value="">Custom…</option>';
$('tzsel').onchange=()=>{if($('tzsel').value)$('tz').value=$('tzsel').value};
$('tz').oninput=tzSync;$('ent').onchange=vis;$('mode').onchange=vis;
COLS.forEach(k=>$(k).oninput=()=>showQ(k));
async function load(){
 const C=await (await fetch('/config.json')).json();
 $('res').innerHTML=C.resList.map((n,i)=>`<option value="${i}">${esc(n)}</option>`).join('');
 $('aspect').innerHTML=C.aspList.map((n,i)=>`<option value="${i}">${esc(n)}</option>`).join('');
 for(const k of ['mode','res','aspect','tz','ntp1','ntp2','rtct','sda','scl','ssid','eapid','eapuser','eapca','host'])$(k).value=C[k];
 for(const k of ['ghost','h24','secs','blink','jump','ntp','rtc'])$(k).checked=C[k];
 COLS.forEach(k=>{$(k).value=C[k];showQ(k)});
 $('ent').value=C.ent?'1':'0';vis();tzSync();
}
$('f').onsubmit=async e=>{e.preventDefault();COLS.forEach(k=>$(k).value=q($(k).value));
 $('msg').textContent='Saving…';
 const j=await (await fetch('/save',{method:'POST',body:new URLSearchParams(new FormData($('f')))})).json();
 $('msg').textContent=j.ok?(j.reboot?'Saved, rebooting…':'Saved'):('Error: '+j.err);
 if(j.ok&&j.reboot)setTimeout(()=>location.reload(),10000)};
$('scan').onclick=async()=>{$('scan').textContent='Scanning…';const l=await (await fetch('/scan')).json();
 $('nets').innerHTML=l.map(n=>`<option value="${esc(n.ssid)}">${n.rssi} dBm${n.ent?' (enterprise)':''}</option>`).join('');
 $('scan').textContent='Scan ('+l.length+' found)'};
$('setnow').onclick=async()=>{await fetch('/settime',{method:'POST',body:new URLSearchParams({epoch:Math.floor(Date.now()/1000)})});st()};
$('up').onclick=()=>{const f=$('fw').files[0];if(!f)return;const d=new FormData();d.append('fw',f);const x=new XMLHttpRequest();
 x.upload.onprogress=e=>$('pg').value=e.loaded/e.total*100;x.onload=()=>{$('msg').textContent=x.responseText;setTimeout(()=>location.reload(),15000)};
 x.open('POST','/update');x.send(d)};
$('rb').onclick=()=>{fetch('/reboot',{method:'POST'});$('msg').textContent='Rebooting…';setTimeout(()=>location.reload(),10000)};
async function st(){try{const s=await (await fetch('/status')).json();
 $('st').textContent=`Time  ${s.time}  [source: ${s.src}]\nWi-Fi ${s.wifi}  IP ${s.ip}  RSSI ${s.rssi} dBm\nScan  ${s.scan}\nRTC   ${s.rtc}\nVideo ${s.video}   heap ${s.heap} B free`}catch(e){}}
load();st();setInterval(st,5000);
</script></body></html>)HTML";
