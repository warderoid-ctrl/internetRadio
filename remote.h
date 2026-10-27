#pragma once
// Phone remote + hooks for Home Assistant, served by the hub itself:  http://crowhub.local/?k=<token shown on the More page>
//   - control the radio, start timers, put a message on the screen, make the hub speak
//   - upload photos (shrunk in your phone's browser, whole photo kept) for the photo frame
//   - see the agenda, add / remove calendar links (secret iCal links, kept on the hub only)
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
<div class=card><h2>AGENDA</h2><div id=agl class=m>Loading...</div></div>
<div class=card><h2>CALENDARS</h2><div id=cl></div><input type=text id=cu placeholder="Paste a secret iCal link (https://... or webcal://...)">
<div class=row><button class=p onclick="cadd()">Add calendar</button></div>
<div class=m>Google: calendar settings &gt; Integrate calendar &gt; <b>Secret address in iCal format</b>.<br>
Outlook: Settings &gt; Calendar &gt; Shared calendars &gt; Publish &gt; <b>ICS</b> link.<br>
iCloud: share the calendar &gt; <b>Public Calendar</b> &gt; copy link.<br>Kept on the hub only - not in the code.</div></div>
<div class=card><h2>PHOTO FRAME</h2><div class=m id=pc></div><div id=pl></div>
<input type=file id=f accept="image/*" multiple style="display:none" onchange="up(this.files)"><div class=row>
<button class=p onclick="f.click()">Add photos</button><button onclick="c('ambient')">Start frame</button></div><div class=m id=us></div></div>
<script>
const K=new URLSearchParams(location.search).get('k')||'';const enc=encodeURIComponent;
async function j(u){const r=await fetch(u+(u.includes('?')?'&':'?')+'k='+K);return r.json()}
async function c(x){await fetch('/api/cmd?c='+x+'&k='+K);setTimeout(s,400)}
async function s(){try{const d=await j('/api/state');st.textContent=d.station;ti.textContent=d.title||d.status;vol.value=d.vol;
sts.innerHTML=d.stations.map((n,i)=>'<button onclick="c(\'station&i='+i+'\')">'+n+'</button>').join('')}catch(e){}}
async function ph(){try{const d=await j('/api/photos');pc.innerHTML=d.photos.length+' photos on the hub ('+Math.round(d.free/1024)+' KB free)'+(d.free<1048576?' - <b>nearly full: tap a photo to delete it</b>':'');
pl.innerHTML=d.photos.map(n=>'<span onclick="del(\''+n+'\')"><img class=th src="/photo?n='+enc(n)+'&k='+K+'"></span>').join('')}catch(e){}}
async function del(n){if(confirm('Delete this photo?')){await fetch('/photo/del?n='+enc(n)+'&k='+K);ph()}}
// photo metadata, read here because shrinking (canvas) drops it: EXIF date + GPS, else a date in the file name
// (WhatsApp strips EXIF but names files IMG-20230814-WA0012.jpg); then the place name and that day's weather
function tiff(b,s){const le=b.getUint16(s)==0x4949,u16=p=>b.getUint16(p,le),u32=p=>b.getUint32(p,le);
const ifd=p=>{const n=u16(s+p),r={};for(let i=0;i<n;i++){const e=s+p+2+i*12;r[u16(e)]={c:u32(e+4),v:e+8}}return r};
const str=x=>{const p=x.c>4?s+u32(x.v):x.v;let t='';for(let i=0;i<x.c-1;i++)t+=String.fromCharCode(b.getUint8(p+i));return t};
const rat=(x,i)=>{const p=s+u32(x.v)+i*8;return u32(p)/u32(p+4)},dms=x=>rat(x,0)+rat(x,1)/60+rat(x,2)/3600;
const o={},i0=ifd(u32(s+4));
if(i0[0x8769]){const ex=ifd(u32(i0[0x8769].v)),d=ex[0x9003]||ex[0x9004];if(d)o.t=str(d)}
if(!o.t&&i0[0x132])o.t=str(i0[0x132]);
if(i0[0x8825]){const g=ifd(u32(i0[0x8825].v));if(g[2]&&g[4]){o.lat=dms(g[2])*(str(g[1])=='S'?-1:1);o.lon=dms(g[4])*(str(g[3])=='W'?-1:1);if(!o.lat&&!o.lon){delete o.lat;delete o.lon}}}
if(o.t){const m=o.t.match(/^(\d{4}):(\d\d):(\d\d) (\d\d):(\d\d)/);o.t=m&&m[1]>'1970'?m[1]+'-'+m[2]+'-'+m[3]+'T'+m[4]+':'+m[5]:undefined}return o}
async function exif(file){try{const b=new DataView(await file.slice(0,262144).arrayBuffer());if(b.getUint16(0)!=0xFFD8)return{};
for(let o=2;o<b.byteLength-10;){const m=b.getUint16(o),L=b.getUint16(o+2);if(m==0xFFE1&&b.getUint32(o+4)==0x45786966)return tiff(b,o+10);
if((m&0xFF00)!=0xFF00||m==0xFFDA)break;o+=2+L}}catch(e){}return{}}
function nameDate(n){const m=n.match(/(?:^|\D)((?:19|20)\d\d)[-_]?(0[1-9]|1[0-2])[-_]?(0[1-9]|[12]\d|3[01])(?:[-_ T]?([01]\d|2[0-3])[-_.:]?([0-5]\d))?/);
return m?m[1]+'-'+m[2]+'-'+m[3]+(m[4]?'T'+m[4]+':'+m[5]:''):undefined}
function iso(ms){const d=new Date(ms),p=x=>String(x).padStart(2,'0');return d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+p(d.getDate())+'T'+p(d.getHours())+':'+p(d.getMinutes())}
// last resort: the file's modified time (on phones usually when it was taken / received), unless it's just now
async function meta(file){const m=await exif(file);if(m.t)m.s='photo';if(!m.t){m.t=nameDate(file.name);if(m.t)m.s='name'}
if(!m.t&&file.lastModified&&Date.now()-file.lastModified>36e5){m.t=iso(file.lastModified);m.s='file'}
if(m.lat!==undefined){try{const r=await(await fetch('https://api.bigdatacloud.net/data/reverse-geocode-client?latitude='+m.lat+'&longitude='+m.lon+'&localityLanguage=en')).json();
const a=r.city||r.locality||r.principalSubdivision;m.p=[a,r.countryName].filter(x=>x).join(', ')}catch(e){}
if(m.t){try{const d=m.t.slice(0,10),r=await(await fetch('https://archive-api.open-meteo.com/v1/archive?latitude='+m.lat+'&longitude='+m.lon+'&start_date='+d+'&end_date='+d+'&daily=weather_code,temperature_2m_max&timezone=auto')).json();
if(r.daily&&r.daily.weather_code[0]!=null){m.wc=r.daily.weather_code[0];m.tx=Math.round(r.daily.temperature_2m_max[0])}}catch(e){}}}return m}
async function up(files){let i=0,found=0,log=[];for(const file of files){us.textContent='Uploading '+(++i)+' of '+files.length+'...';
const m=await meta(file);if(m.t||m.p)found++;
log.push(esc(file.name)+': '+(m.t?'date from '+({photo:'photo',name:'file name',file:'file time'})[m.s]:'no date')+', '+(m.p?esc(m.p):m.lat!==undefined?'location (no place name)':'no location'));
const bm=await createImageBitmap(file);const W=bm.width,H=bm.height;
const sc=Math.min(1,Math.max(800/W,480/H),1600/Math.max(W,H));const cv=document.createElement('canvas');
cv.width=Math.round(W*sc);cv.height=Math.round(H*sc);cv.getContext('2d').drawImage(bm,0,0,cv.width,cv.height);
const b=await new Promise(r=>cv.toBlob(r,'image/jpeg',.85));
// the date goes in the name too (d202308141822_...), so the hub can find "on this day" photos without opening them
const base=(m.t?'d'+m.t.replace(/\D/g,'').padEnd(12,'0').slice(0,12)+'_':'p')+Date.now()+'_'+i;
const fd=new FormData();fd.append('file',b,base+'.jpg');
if(m.t||m.p){m.lat=m.lat&&+m.lat.toFixed(4);m.lon=m.lon&&+m.lon.toFixed(4);fd.append('file',new Blob([JSON.stringify(m)],{type:'application/json'}),base+'.json')}
const r=await fetch('/upload?k='+K,{method:'POST',body:fd});if(!r.ok||(await r.text())!='ok')log.push('<b>'+esc(file.name)+': not saved - hub full?</b>')}
us.innerHTML='Done - '+found+' of '+files.length+' had a date or place<br>'+log.join('<br>');ph()}
async function ag(){try{const d=await j('/api/agenda');agl.innerHTML=d.cal?(d.items.length?d.items.map(a=>'<div style="margin:8px 0"><b>'+esc(a.t)+'</b><br>'+esc(a.w)+(a.l?' - '+esc(a.l):'')+'</div>').join(''):'Nothing in the next two weeks'):'No calendar yet - add one below';
cl.innerHTML=d.links.map((n,i)=>'<div class=row style="align-items:center"><span class=m style="flex:1">'+esc(n)+'</span><button onclick="cdel('+i+')">Remove</button></div>').join('')}catch(e){}}
function esc(t){return String(t).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
async function cadd(){if(!cu.value)return;const r=await j('/api/cal/add?u='+enc(cu.value));if(!r.ok)alert(r.err||'Could not add');cu.value='';setTimeout(ag,300);setTimeout(ag,8000)}
async function cdel(i){if(confirm('Remove this calendar?')){await j('/api/cal/del?i='+i);setTimeout(ag,300)}}
s();ph();ag();setInterval(s,5000);setInterval(ag,60000)
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
  String o = "{\"station\":\"" + jsonEsc(curSt.name) + "\",\"title\":\"";
  if (xSemaphoreTake(metaMx, 20) == pdTRUE) { o += jsonEsc(metaTitle); xSemaphoreGive(metaMx); }
  o += "\",\"status\":\"" + String(playState == 2 ? "Live" : playState == 1 ? "Connecting" : playState == 3 ? "Problem" : "Stopped") + "\",\"vol\":" + String(volume) + ",\"stations\":[";
  for (int i = 0; i < nFavs; i++) { if (i) o += ","; o += "\"" + jsonEsc(favs[i].name) + "\""; }
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
  if (root && root.isDirectory()) for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (!isImageName(f.name())) continue;                     // not the .json metadata next to each photo
    if (!first) o += ",";
    first = false;
    o += "\"" + jsonEsc(f.name()) + "\"";
  }
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
  String p = "/photos/" + safeName(server.arg("n"));
  LittleFS.remove(p);
  int dot = p.lastIndexOf('.');
  String meta = dot > 0 ? p.substring(0, dot) + ".json" : String();
  if (meta.length() && LittleFS.exists(meta)) LittleFS.remove(meta);   // and its metadata, if it had any
  server.send(200, "application/json", "{\"ok\":true}");
}
static bool upFailed = false;                                // a write came up short (flash full): say so, keep nothing
static String upPath;
static void hUploadDone() {
  if (!upAuthOk) { server.send(401, "text/plain", "denied"); return; }
  server.send(upFailed ? 507 : 200, "text/plain", upFailed ? "full" : "ok");
}
static void hUpload() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    upAuthOk = authed();
    if (!upAuthOk) return;
    // a request is one photo, then maybe its .json: start clean on the photo, skip the .json if the photo didn't fit
    if (!u.filename.endsWith(".json")) upFailed = false;
    upPath = "";
    if (upFailed) return;
    LittleFS.mkdir("/photos");
    upPath = "/photos/" + safeName(u.filename);
    upFile = LittleFS.open(upPath, FILE_WRITE);
    if (!upFile) upFailed = true;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (upAuthOk && upFile && upFile.write(u.buf, u.currentSize) != u.currentSize) upFailed = true;
  } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
    if (upFile) upFile.close();
    if ((upFailed || u.status == UPLOAD_FILE_ABORTED) && upPath.length()) LittleFS.remove(upPath);
  }
}

static void hAgenda() {                                       // upcoming appointments + the calendar links (masked)
  if (!authed()) return deny();
  static Appt *local = (Appt *)heap_caps_malloc(sizeof(Appt) * MAX_APPT, MALLOC_CAP_SPIRAM);
  int n = 0;
  if (local) { DATA_LOCK(); n = nAppts; memcpy(local, appts, sizeof(Appt) * n); DATA_UNLOCK(); }
  time_t now = time(nullptr);
  String o = "{\"cal\":" + String(agendaConfigured || nCal ? "true" : "false") + ",\"items\":[";
  bool first = true;
  for (int i = 0; i < n; i++) {
    if (local[i].end < now) continue;
    char w[48]; fmtWhen(local[i], w, sizeof(w));
    if (!first) o += ",";
    first = false;
    o += "{\"t\":\"" + jsonEsc(local[i].title) + "\",\"w\":\"" + jsonEsc(w) + "\",\"l\":\"" + jsonEsc(local[i].loc) + "\"}";
  }
  o += "],\"links\":[";
  for (int i = 0; i < nCal; i++) { char l[96]; calLabel(i, l, sizeof(l)); if (i) o += ","; o += "\"" + jsonEsc(l) + "\""; }
  o += "]}";
  server.send(200, "application/json", o);
}
static void hCalAdd() {
  if (!authed()) return deny();
  bool ok = calAdd(server.arg("u").c_str());
  server.send(200, "application/json", ok ? "{\"ok\":true}" : (nCal >= MAX_CAL ? "{\"ok\":false,\"err\":\"Up to 4 calendars\"}" : "{\"ok\":false,\"err\":\"That doesn't look like a calendar link\"}"));
}
static void hCalDel() {
  if (!authed()) return deny();
  calDel(server.arg("i").toInt());
  server.send(200, "application/json", "{\"ok\":true}");
}

static bool remoteStarted = false;
static void remoteBegin() {
  if (remoteStarted) return;
  remoteStarted = true;
  if (MDNS.begin("crowhub")) MDNS.addService("http", "tcp", 80);
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");           // a phone must never run an old copy of the page
    server.send(200, "text/html", REMOTE_HTML);
  });
  server.on("/api/state", HTTP_GET, hState);
  server.on("/api/cmd", HTTP_GET, hCmd);
  server.on("/api/photos", HTTP_GET, hPhotos);
  server.on("/api/agenda", HTTP_GET, hAgenda);
  server.on("/api/cal/add", HTTP_GET, hCalAdd);
  server.on("/api/cal/del", HTTP_GET, hCalDel);
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
