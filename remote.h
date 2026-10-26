#pragma once
// Phone remote + hooks for Home Assistant, served by the hub itself:  http://crowhub.local/?k=<token shown on the More page>
//   - control the radio, start timers, put a message on the screen, make the hub speak
//   - upload photos (shrunk in your phone's browser, whole photo kept) for the photo frame
//   - HA hook:  GET /notify?k=..&msg=Doorbell&say=1   -> banner + beeps (+ spoken) on the hub

#if REMOTE_ENABLED
static WebServer server(80);
static File upFile;
static bool upAuthOk = false;

static const char REMOTE_HTML[] = R"HTML(<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>Crow Hub</title><style>
:root{--bg:#0b0c10;--c:#15171f;--c2:#1e212c;--t:#f2f3f7;--m:#8b90a0;--a:#ff6b3d;--a2:#ff3d6e}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--t);font:16px system-ui,sans-serif;padding:14px;max-width:560px;margin:auto}
h1{font-size:20px;letter-spacing:.15em;color:var(--m);margin:6px 0 14px}h2{font-size:13px;letter-spacing:.2em;color:var(--a);margin:0 0 10px}
.card{background:var(--c);border-radius:22px;padding:16px;margin-bottom:12px}
button{background:var(--c2);color:var(--t);border:0;border-radius:16px;padding:14px 16px;font-size:16px;margin:3px}
button.p{background:linear-gradient(var(--a),var(--a2))}button:active{opacity:.6}
.row{display:flex;flex-wrap:wrap;gap:2px}.big{font-size:22px;font-weight:600}.m{color:var(--m);font-size:14px}
input[type=range]{width:100%;accent-color:var(--a)}input[type=text]{width:100%;padding:14px;border-radius:14px;border:0;background:var(--c2);color:var(--t);font-size:16px}
img.th{width:76px;height:46px;object-fit:cover;border-radius:8px;margin:3px}</style></head><body>
<h1>CROW HUB</h1>
<div class=card><h2>NOW PLAYING</h2><div class=big id=st>-</div><div class=m id=ti>&nbsp;</div>
<div class=row><button onclick="c('prev')">&#9198;</button><button class=p onclick="c('pp')">&#9199; Play / pause</button><button onclick="c('next')">&#9197;</button></div>
<input type=range min=0 max=21 id=vol onchange="c('vol&v='+this.value)"><div class=row id=sts></div></div>
<div class=card><h2>TIMER</h2><div class=row><button onclick="c('timer&secs=180')">Tea 3</button><button onclick="c('timer&secs=360')">Egg 6</button><button onclick="c('timer&secs=600')">10 min</button><button onclick="c('timer&secs=1200')">20 min</button></div></div>
<div class=card><h2>SAY / SHOW</h2><input type=text id=tx placeholder="Dinner's ready!"><div class=row>
<button class=p onclick="c('msg&text='+enc(tx.value))">Show on screen</button><button onclick="c('say&text='+enc(tx.value))">Speak it</button></div></div>
<div class=card><h2>PHOTO FRAME</h2><div class=m id=pc></div><div id=pl></div>
<input type=file id=f accept="image/*" multiple style="display:none" onchange="up(this.files)"><div class=row>
<button class=p onclick="f.click()">Add photos</button><button onclick="c('ambient')">Start frame</button></div><div class=m id=us></div></div>
<script>
const K=new URLSearchParams(location.search).get('k')||'';const enc=encodeURIComponent;
async function j(u){const r=await fetch(u+(u.includes('?')?'&':'?')+'k='+K);return r.json()}
async function c(x){await fetch('/api/cmd?c='+x+'&k='+K);setTimeout(s,400)}
async function s(){try{const d=await j('/api/state');st.textContent=d.station;ti.textContent=d.title||d.status;vol.value=d.vol;
sts.innerHTML=d.stations.map((n,i)=>'<button onclick="c(\'station&i='+i+'\')">'+n+'</button>').join('')}catch(e){}}
async function ph(){try{const d=await j('/api/photos');pc.textContent=d.photos.length+' photos on the hub ('+Math.round(d.free/1024)+' KB free)';
pl.innerHTML=d.photos.map(n=>'<span onclick="del(\''+n+'\')"><img class=th src="/photo?n='+enc(n)+'&k='+K+'"></span>').join('')}catch(e){}}
async function del(n){if(confirm('Delete this photo?')){await fetch('/photo/del?n='+enc(n)+'&k='+K);ph()}}
async function up(files){let i=0;for(const file of files){us.textContent='Uploading '+(++i)+' of '+files.length+'...';
const bm=await createImageBitmap(file);const W=bm.width,H=bm.height;
const sc=Math.min(1,Math.max(800/W,480/H),1600/Math.max(W,H));const cv=document.createElement('canvas');
cv.width=Math.round(W*sc);cv.height=Math.round(H*sc);cv.getContext('2d').drawImage(bm,0,0,cv.width,cv.height);
const b=await new Promise(r=>cv.toBlob(r,'image/jpeg',.85));const fd=new FormData();fd.append('file',b,'p'+Date.now()+'_'+i+'.jpg');
await fetch('/upload?k='+K,{method:'POST',body:fd})}us.textContent='Done';ph()}
s();ph();setInterval(s,5000)
</script></body></html>)HTML";

static String remoteToken;   // random per device, generated on first boot and kept in NVS
static void remoteTokenInit() {
  remoteToken = prefs.getString("rtok", "");
  if (remoteToken.length()) return;
  static const char cs[] = "abcdefghjkmnpqrstuvwxyz23456789";
  for (int i = 0; i < 10; i++) remoteToken += cs[esp_random() % (sizeof(cs) - 1)];
  prefs.putString("rtok", remoteToken);
}
static bool authed() { return remoteToken.length() && server.arg("k") == remoteToken; }
static void deny() { server.send(401, "text/plain", "need ?k=token"); }

static String jsonEsc(const char *s) { String o; for (; *s; s++) { if (*s == '"' || *s == '\\') o += '\\'; if ((uint8_t)*s >= 32) o += *s; } return o; }

static void hState() {
  if (!authed()) return deny();
  String o = "{\"station\":\"" + jsonEsc(STATIONS[curStation].name) + "\",\"title\":\"";
  if (xSemaphoreTake(metaMx, 20) == pdTRUE) { o += jsonEsc(metaTitle); xSemaphoreGive(metaMx); }
  o += "\",\"status\":\"" + String(playState == 2 ? "Live" : playState == 1 ? "Connecting" : playState == 3 ? "Problem" : "Stopped") + "\",\"vol\":" + String(volume) + ",\"stations\":[";
  for (int i = 0; i < N_STATIONS; i++) { if (i) o += ","; o += "\"" + jsonEsc(STATIONS[i].name) + "\""; }
  o += "]}";
  server.send(200, "application/json", o);
}

static void hCmd() {
  if (!authed()) return deny();
  String c = server.arg("c");
  if (c == "pp") onPlayPause(nullptr);
  else if (c == "next") playStation(curStation + 1);
  else if (c == "prev") playStation(curStation - 1);
  else if (c == "vol") setVolume(server.arg("v").toInt());
  else if (c == "station") playStation(server.arg("i").toInt());
  else if (c == "timer") { int s = server.arg("secs").toInt(); if (s > 0) timerStart(s, "Remote"); }
  else if (c == "msg") { postUi(UM_TOAST, server.arg("text").c_str(), C_ACCENT); beep(2); }
  else if (c == "say") { String t = server.arg("text"); if (t.length()) voiceSpeak(t.c_str()); }
  else if (c == "ambient") ambientEnter();
  else if (c == "page") showPage(server.arg("i").toInt());
  server.send(200, "application/json", "{\"ok\":true}");
}

static void hNotify() {                                       // for Home Assistant rest_command / automations
  if (!authed()) return deny();
  String m = server.arg("msg");
  if (m.length()) { postUi(UM_TOAST, m.c_str(), C_WARN); beep(3); if (server.arg("say") == "1") voiceSpeak(m.c_str()); }
  server.send(200, "text/plain", "ok");
}

static void hPhotos() {
  if (!authed()) return deny();
  String o = "{\"photos\":[";
  File root = LittleFS.open("/photos");
  bool first = true;
  if (root && root.isDirectory()) for (File f = root.openNextFile(); f; f = root.openNextFile()) { if (!first) o += ","; first = false; o += "\"" + jsonEsc(f.name()) + "\""; }
  o += "],\"free\":" + String(LittleFS.totalBytes() - LittleFS.usedBytes()) + "}";
  server.send(200, "application/json", o);
}
static String safeName(const String &n) {
  String o;
  for (size_t i = 0; i < n.length() && o.length() < 40; i++) { char c = n[i]; o += (isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_') ? c : '_'; }
  return o;
}
static void hPhotoGet() {
  if (!authed()) return deny();
  File f = LittleFS.open("/photos/" + safeName(server.arg("n")), "r");
  if (!f) { server.send(404, "text/plain", "no"); return; }
  server.streamFile(f, "image/jpeg");
  f.close();
}
static void hPhotoDel() {
  if (!authed()) return deny();
  LittleFS.remove("/photos/" + safeName(server.arg("n")));
  server.send(200, "application/json", "{\"ok\":true}");
}
static void hUploadDone() { server.send(upAuthOk ? 200 : 401, "text/plain", upAuthOk ? "ok" : "denied"); }
static void hUpload() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    upAuthOk = authed();
    if (!upAuthOk) return;
    LittleFS.mkdir("/photos");
    upFile = LittleFS.open("/photos/" + safeName(u.filename), FILE_WRITE);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (upAuthOk && upFile) upFile.write(u.buf, u.currentSize);
  } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
    if (upFile) upFile.close();
  }
}

static bool remoteStarted = false;
static void remoteBegin() {
  if (remoteStarted) return;
  remoteStarted = true;
  if (MDNS.begin("crowhub")) MDNS.addService("http", "tcp", 80);
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", REMOTE_HTML); });
  server.on("/api/state", HTTP_GET, hState);
  server.on("/api/cmd", HTTP_GET, hCmd);
  server.on("/api/photos", HTTP_GET, hPhotos);
  server.on("/notify", HTTP_ANY, hNotify);
  server.on("/photo", HTTP_GET, hPhotoGet);
  server.on("/photo/del", HTTP_GET, hPhotoDel);
  server.on("/upload", HTTP_POST, hUploadDone, hUpload);
  server.begin();
}
static void remoteTick() { if (remoteStarted) server.handleClient(); }
#else
static void remoteBegin() {}
static void remoteTick() {}
#endif
