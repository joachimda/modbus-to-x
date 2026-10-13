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
var protectedMode=false,protectionKnown=false,token=null,operationId=0,sequence=0,source=null,pollTimer=null,retryTimer=null,awaitUpload=null;
function s(msg,ok){var e=document.getElementById('status');e.style.display='block';e.className=ok?'ok':'err';e.textContent=msg;}
function encodedPassword(value){var bytes=new TextEncoder().encode(value),binary='';for(var i=0;i<bytes.length;i++)binary+=String.fromCharCode(bytes[i]);return btoa(binary);}
function parse(r){if(r.status===204)return {};return r.json();}
async function session(){
  if(token)return token;var headers=new Headers({'X-MBX-Request':'1'}),response=await fetch('/api/system/ota/session',{method:'POST',cache:'no-store',headers:headers});
  if(response.status===401){var password=prompt('OTA password:');if(password===null||password==='')throw new Error('password required');headers.set('Authorization','Bearer '+encodedPassword(password));response=await fetch('/api/system/ota/session',{method:'POST',cache:'no-store',headers:headers});}
  if(!response.ok)throw new Error(response.status+' '+response.statusText);var body=await parse(response);if(!/^[0-9a-fA-F]{64}$/.test(body.token||''))throw new Error('invalid OTA session');token=body.token;return token;
}
async function api(url,options){
  options=options||{};var headers=new Headers(options.headers||{});headers.set('X-MBX-Request','1');headers.set('X-OTA-Session',await session());options.headers=headers;options.cache='no-store';
  var response=await fetch(url,options);if(response.status===401){token=null;headers.set('X-OTA-Session',await session());response=await fetch(url,options);}
  if(!response.ok)throw new Error(response.status+' '+response.statusText);var body=await parse(response);if(url==='/api/system/ota/password')token=null;return body;
}
function active(state){return ['quiescing','awaiting_upload','downloading_writing_firmware','verifying_firmware','downloading_writing_filesystem','verifying_filesystem','failure_detected','resuming','rebooting'].indexOf(state)>=0;}
function progress(snapshot){
  var labels={idle:'Ready for an update',quiescing:'Preparing device services',awaiting_upload:'Ready for image upload',downloading_writing_firmware:'Downloading and writing firmware',verifying_firmware:'Verifying firmware',downloading_writing_filesystem:'Downloading and writing filesystem',verifying_filesystem:'Verifying filesystem',failure_detected:'Update failure detected',resuming:'Restoring device services',failed:'Update failed',rebooting:'Update verified; rebooting'};
  var msg=labels[snapshot.state]||'Update status';if(snapshot.targetVersion)msg+=' to '+snapshot.targetVersion;if(snapshot.component&&snapshot.component!=='none'){
    if(snapshot.totalBytes>0)msg+=' — '+snapshot.receivedBytes+' / '+snapshot.totalBytes+' bytes';else if(snapshot.receivedBytes>0)msg+=' — '+snapshot.receivedBytes+' bytes';if(snapshot.componentPercentKnown)msg+=' ('+snapshot.componentPercent+'%)';}
  if(snapshot.overallPercentKnown)msg+=' — overall '+snapshot.overallPercent+'%';if(snapshot.state==='failed'&&snapshot.error)msg+=': '+snapshot.error;return msg;
}
function accept(snapshot){
  var op=Number(snapshot.operationId),next=Number(snapshot.sequence);if(!Number.isInteger(op)||!Number.isInteger(next)||op<=0||next<=sequence||(operationId&&op!==operationId))return false;operationId=op;sequence=next;s(progress(snapshot),snapshot.state!=='failed');
  if(snapshot.state==='awaiting_upload'&&awaitUpload){awaitUpload.resolve();awaitUpload=null;}
  if(snapshot.state==='failed'){if(awaitUpload){awaitUpload.reject(new Error(snapshot.error||'OTA failed'));awaitUpload=null;}token=null;stopMonitor();}
  if(snapshot.state==='rebooting'){token=null;stopMonitor();s('Update verified; device is rebooting. Reconnect when it returns.',true);}return true;
}
async function poll(){if(!operationId||source)return;try{var response=await fetch('/api/system/ota/status',{cache:'no-store'});if(response.ok)accept(await parse(response));}catch(e){}if(operationId&&!source)pollTimer=setTimeout(poll,3000);}
function openEvents(){
  if(!operationId)return;if(source)source.close();source=new EventSource('/api/system/ota/events');source.addEventListener('ota-progress',function(event){try{accept(JSON.parse(event.data||'{}'));}catch(e){}});source.addEventListener('error',function(){source.close();source=null;poll();retryTimer=setTimeout(openEvents,5000);});
}
function monitor(op){stopMonitor();operationId=op;sequence=0;openEvents();if(!source||source.readyState!==1)poll();}
function stopMonitor(){if(source)source.close();source=null;if(pollTimer)clearTimeout(pollTimer);if(retryTimer)clearTimeout(retryTimer);pollTimer=null;retryTimer=null;operationId=0;sequence=0;}
async function recover(){try{var response=await fetch('/api/system/ota/status',{cache:'no-store'}),snapshot=await parse(response);if(response.ok&&active(snapshot.state)&&snapshot.operationId>0){monitor(snapshot.operationId);accept(snapshot);return true;}}catch(e){}return false;}
function waitForUpload(){return new Promise(function(resolve,reject){awaitUpload={resolve:resolve,reject:reject};});}
async function loadProtection(){
  try{var response=await fetch('/api/system/ota/http/settings',{cache:'no-store'});if(!response.ok)throw new Error(response.status+' '+response.statusText);var d=await parse(response);protectedMode=!!d.passwordProtected;protectionKnown=true;
    document.getElementById('protection').textContent=protectedMode?'Protected — OTA operations require the configured password.':'Unprotected — confirm uploads carefully and set a password when possible.';
    document.getElementById('setPassBtn').textContent=protectedMode?'Change password':'Set password';document.getElementById('clearPassBtn').disabled=!protectedMode;
  }catch(e){protectionKnown=false;document.getElementById('protection').textContent='Protection state is unavailable while an update is active.';document.getElementById('setPassBtn').disabled=true;document.getElementById('clearPassBtn').disabled=true;}
}
async function setPassword(){
  var next=document.getElementById('newPass').value,confirmValue=document.getElementById('confirmPass').value;
  var bytes=new TextEncoder().encode(next).length;if(next!==confirmValue){s('Passwords do not match',false);return;}if(bytes<8||bytes>128){s('Password must be 8–128 UTF-8 bytes',false);return;}
  try{await api('/api/system/ota/password',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:next})});document.getElementById('newPass').value='';document.getElementById('confirmPass').value='';s('OTA password saved',true);loadProtection();}catch(e){s('Password change failed: '+e,false);}
}
async function clearPassword(){
  if(!protectedMode||!confirm('Clear the OTA password and leave recovery unprotected?'))return;
  try{await api('/api/system/ota/password',{method:'DELETE'});s('OTA password cleared',true);loadProtection();}catch(e){s('Password clear failed: '+e,false);}
}
async function otaCheck(){
  s('Checking...',true);try{var d=await api('/api/system/ota/http/check',{method:'POST'});if(d.available){s('Update available: '+d.version,true);document.getElementById('applyBtn').disabled=false;}else if(d.pending){s('Check in progress; try again shortly',true);}else if(d.ok){s('No update available',true);}else{s('Check failed: '+(d.error||'unknown'),false);}}catch(e){s('Check failed: '+e,false);}
}
async function otaApply(){
  if(!confirm('Apply the signed update? The device will reboot.'))return;s('Preparing update...',true);document.getElementById('applyBtn').disabled=true;
  try{var d=await api('/api/system/ota/http/apply',{method:'POST'});if(!d.operationId)throw new Error('update was not admitted');monitor(d.operationId);}catch(e){if(!(await recover())){s('Apply failed: '+e,false);document.getElementById('applyBtn').disabled=false;}}
}
async function upload(type){
  var firmware=type==='firmware',input=document.getElementById(firmware?'fwFile':'fsFile'),file=input.files[0];if(!file){s('Select a .bin file first',false);return;}
  var warning=!protectionKnown?'Protection state is unavailable; the server still enforces its configured policy.':(protectedMode?'The configured OTA password is required.':'WARNING: OTA protection is not configured.');if(!confirm('Upload '+file.name+' as the '+type+' image?\n\n'+warning+'\nThe device will reboot on success.'))return;
  s('Preparing '+type+' upload...',true);var start;try{start=await api('/api/system/ota/manual/start',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({component:type,size:file.size})});}
  catch(e){if(!(await recover())){s('Upload failed: '+e,false);return;}start={operationId:operationId};}
  try{var ready=waitForUpload();monitor(start.operationId);await ready;var form=new FormData();form.append(type,file,file.name);var headers=new Headers({'X-MBX-Request':'1','X-OTA-Session':await session(),'X-OTA-Operation':String(start.operationId)}),response=await fetch(firmware?'/api/system/ota/firmware':'/api/system/ota/fs',{method:'POST',body:form,headers:headers});if(!response.ok)throw new Error(response.status+' '+response.statusText);input.value='';s('Upload complete. Device rebooting...',true);}
  catch(e){if(!(await recover()))s('Upload failed: '+e,false);}
}
recover();loadProtection();
</script></body></html>)rawhtml";

#endif
