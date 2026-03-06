#ifndef MBX_OTA_RECOVERY_PAGE_H
#define MBX_OTA_RECOVERY_PAGE_H

#include <pgmspace.h>

static const char OTA_RECOVERY_HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MBX Recovery</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em;background:#1a1a2e;color:#e0e0e0}
h1{color:#e94560}
.card{background:#16213e;border-radius:8px;padding:1.2em;margin:1em 0}
button{background:#e94560;color:#fff;border:none;padding:.7em 1.4em;border-radius:4px;cursor:pointer;font-size:1em;width:100%;margin:.3em 0}
button:disabled{opacity:.5;cursor:wait}
#status{margin-top:1em;padding:.8em;border-radius:4px;display:none}
.ok{background:#0f3460;color:#4ecca3}.err{background:#3a0000;color:#e94560}
input[type=file]{margin:.5em 0;width:100%;box-sizing:border-box}
</style>
</head><body>
<h1>MBX Recovery</h1>
<div class="card">
<p>The web UI filesystem appears to be missing or corrupted.
The device backend is running normally.</p>
</div>
<div class="card">
<h3>OTA Update from GitHub</h3>
<button onclick="otaCheck()">Check for Update</button>
<button onclick="otaApply()" id="applyBtn" disabled>Apply Update</button>
</div>
<div class="card">
<h3>Manual Filesystem Upload</h3>
<input type="file" id="fsFile" accept=".bin">
<button onclick="uploadFs()">Upload Filesystem</button>
</div>
<div id="status"></div>
<script>
var AUTH='Basic '+btoa('admin:admin');
function s(msg,ok){var e=document.getElementById('status');e.style.display='block';e.className=ok?'ok':'err';e.textContent=msg;}
function otaCheck(){
  s('Checking...',true);
  fetch('/api/system/ota/http/check',{method:'POST',headers:{Authorization:AUTH}})
  .then(r=>r.json()).then(d=>{
    if(d.available){s('Update available: v'+d.version,true);document.getElementById('applyBtn').disabled=false;}
    else if(d.pending){s('Check in progress, try again shortly',true);}
    else if(d.ok){s('No update available',true);}
    else{s('Check failed: '+(d.error||'unknown'),false);}
  }).catch(e=>s('Error: '+e,false));
}
function otaApply(){
  s('Applying update... device will reboot',true);
  document.getElementById('applyBtn').disabled=true;
  fetch('/api/system/ota/http/apply',{method:'POST',headers:{Authorization:AUTH}})
  .then(r=>r.json()).then(d=>{
    if(d.ok){s('Update started (v'+d.version+'). Device will reboot in ~60s.',true);}
    else{s('Apply failed: '+(d.error||'unknown'),false);document.getElementById('applyBtn').disabled=false;}
  }).catch(e=>s('Error: '+e,false));
}
function uploadFs(){
  var f=document.getElementById('fsFile').files[0];
  if(!f){s('Select a .bin file first',false);return;}
  s('Uploading filesystem...',true);
  var fd=new FormData();fd.append('fs',f,f.name);
  fetch('/api/system/ota/fs',{method:'POST',headers:{Authorization:AUTH},body:fd})
  .then(r=>r.json()).then(d=>{
    if(d.ok){s('Upload OK. Device rebooting...',true);}
    else{s('Upload failed: '+(d.error||'unknown'),false);}
  }).catch(e=>s('Error: '+e,false));
}
</script>
</body></html>)rawhtml";

#endif
