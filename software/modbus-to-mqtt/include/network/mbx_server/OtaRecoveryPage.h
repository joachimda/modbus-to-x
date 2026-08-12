#ifndef MBX_OTA_RECOVERY_PAGE_H
#define MBX_OTA_RECOVERY_PAGE_H

#include <pgmspace.h>

static const char OTA_RECOVERY_HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MBX Recovery</title>
<style>
body{font-family:sans-serif;max-width:560px;margin:2em auto;padding:0 1em;background:#1a1a2e;color:#e0e0e0}
h1{color:#e94560}.card{background:#16213e;border-radius:8px;padding:1.2em;margin:1em 0}
button{background:#e94560;color:#fff;border:none;padding:.7em 1.4em;border-radius:4px;cursor:pointer;font-size:1em;margin:.3em .2em .3em 0}
button:disabled{opacity:.5;cursor:wait}input{display:block;margin:.5em 0;width:100%;box-sizing:border-box;padding:.5em}
#status{margin-top:1em;padding:.8em;border-radius:4px;display:none}.ok{background:#0f3460;color:#4ecca3}.err{background:#3a0000;color:#ff8798}
.warn{color:#ffd166}.muted{color:#b8bfd3;font-size:.92em}
</style>
</head><body>
<h1>MBX Recovery</h1>
<div class="card"><p>The UI filesystem appears to be missing or corrupted. The compiled recovery tools remain available.</p></div>
<div class="card">
<h3>OTA protection</h3><p id="protection" class="warn">Loading protection state…</p>
<p class="muted">An OTA password is recommended. Plain HTTP does not hide it from observers on the local network.</p>
<input type="password" id="newPass" minlength="8" maxlength="128" placeholder="New password (8–128 bytes)" autocomplete="new-password">
<input type="password" id="confirmPass" minlength="8" maxlength="128" placeholder="Confirm new password" autocomplete="new-password">
<button onclick="setPassword()" id="setPassBtn">Set password</button><button onclick="clearPassword()" id="clearPassBtn">Clear password</button>
</div>
<div class="card">
<h3>Signed update from GitHub</h3>
<button onclick="otaCheck()">Check for update</button><button onclick="otaApply()" id="applyBtn" disabled>Apply update</button>
</div>
<div class="card">
<h3>Manual firmware upload</h3><input type="file" id="fwFile" accept=".bin,application/octet-stream"><button onclick="upload('firmware')">Upload firmware</button>
<h3>Manual filesystem upload</h3><input type="file" id="fsFile" accept=".bin,application/octet-stream"><button onclick="upload('filesystem')">Upload filesystem</button>
</div>
<div id="status"></div>
<script>
var protectedMode=false,password=null;
function s(msg,ok){var e=document.getElementById('status');e.style.display='block';e.className=ok?'ok':'err';e.textContent=msg;}
function askPassword(){var value=prompt('OTA password:');if(value===null||value==='')return false;password=value;return true;}
function encodedPassword(value){var bytes=new TextEncoder().encode(value),binary='';for(var i=0;i<bytes.length;i++)binary+=String.fromCharCode(bytes[i]);return btoa(binary);}
function authHeaders(headers){var h=new Headers(headers||{});if(password!==null)h.set('Authorization','Bearer '+encodedPassword(password));return h;}
function parse(r){if(r.status===204)return {};return r.json();}
async function api(url,options){
  options=options||{};options.headers=authHeaders(options.headers);options.headers.set('X-MBX-Request','1');
  var response=await fetch(url,options);
  if(response.status===401){password=null;if(!askPassword())throw new Error('password required');options.headers=authHeaders(options.headers);response=await fetch(url,options);}
  if(!response.ok)throw new Error(response.status+' '+response.statusText);return parse(response);
}
async function loadProtection(){
  try{var d=await fetch('/api/system/ota/http/settings',{cache:'no-store'}).then(parse);protectedMode=!!d.passwordProtected;
    document.getElementById('protection').textContent=protectedMode?'Protected — OTA operations require the configured password.':'Unprotected — confirm uploads carefully and set a password when possible.';
    document.getElementById('setPassBtn').textContent=protectedMode?'Change password':'Set password';document.getElementById('clearPassBtn').disabled=!protectedMode;
  }catch(e){s('Could not load protection state: '+e,false);}
}
async function setPassword(){
  var next=document.getElementById('newPass').value,confirmValue=document.getElementById('confirmPass').value;
  var bytes=new TextEncoder().encode(next).length;if(next!==confirmValue){s('Passwords do not match',false);return;}if(bytes<8||bytes>128){s('Password must be 8–128 UTF-8 bytes',false);return;}
  if(protectedMode&&password===null&&!askPassword())return;
  try{await api('/api/system/ota/password',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:next})});password=null;document.getElementById('newPass').value='';document.getElementById('confirmPass').value='';s('OTA password saved',true);loadProtection();}catch(e){password=null;s('Password change failed: '+e,false);}
}
async function clearPassword(){
  if(!protectedMode||!confirm('Clear the OTA password and leave recovery unprotected?'))return;if(password===null&&!askPassword())return;
  try{await api('/api/system/ota/password',{method:'DELETE'});password=null;s('OTA password cleared',true);loadProtection();}catch(e){password=null;s('Password clear failed: '+e,false);}
}
async function otaCheck(){
  if(protectedMode&&password===null&&!askPassword())return;s('Checking...',true);
  try{var d=await api('/api/system/ota/http/check',{method:'POST'});if(d.available){s('Update available: '+d.version,true);document.getElementById('applyBtn').disabled=false;}else if(d.pending){s('Check in progress; try again shortly',true);}else if(d.ok){s('No update available',true);}else{s('Check failed: '+(d.error||'unknown'),false);}}catch(e){password=null;s('Check failed: '+e,false);}
}
async function otaApply(){
  if(!confirm('Apply the signed update? The device will reboot.'))return;if(protectedMode&&password===null&&!askPassword())return;s('Applying update...',true);document.getElementById('applyBtn').disabled=true;
  try{var d=await api('/api/system/ota/http/apply',{method:'POST'});password=null;s(d.ok?'Update started. Device will reboot.':'Apply failed: '+(d.error||'unknown'),!!d.ok);}catch(e){password=null;s('Apply failed: '+e,false);document.getElementById('applyBtn').disabled=false;}
}
async function upload(type){
  var firmware=type==='firmware',input=document.getElementById(firmware?'fwFile':'fsFile'),file=input.files[0];if(!file){s('Select a .bin file first',false);return;}
  var warning=protectedMode?'The configured OTA password is required.':'WARNING: OTA protection is not configured.';if(!confirm('Upload '+file.name+' as the '+type+' image?\n\n'+warning+'\nThe device will reboot on success.'))return;
  if(protectedMode&&password===null&&!askPassword())return;var form=new FormData();form.append(type,file,file.name);s('Uploading '+type+'...',true);
  try{var d=await api(firmware?'/api/system/ota/firmware':'/api/system/ota/fs',{method:'POST',body:form});password=null;input.value='';s(d.ok?'Upload complete. Device rebooting...':'Upload failed',!!d.ok);}catch(e){password=null;s('Upload failed: '+e,false);}
}
loadProtection();
</script></body></html>)rawhtml";

#endif
