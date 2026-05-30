#include "web_server.h"
#include "esp_zb_switch.h"
#include "favorites_storage.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "bdb/esp_zigbee_bdb_touchlink.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WEB_SERVER";
static httpd_handle_t server = NULL;

/* ── FAVICON ─────────────────────────────────────────────── */

static const char FAVICON_SVG[] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>"

    "<defs>"
    "<radialGradient id='glow' cx='50%' cy='30%' r='60%'>"
    "<stop offset='0%' stop-color='#ffcc66' stop-opacity='1'/>"
    "<stop offset='60%' stop-color='#ff8800' stop-opacity='0.4'/>"
    "<stop offset='100%' stop-color='#1a0e00' stop-opacity='0'/>"
    "</radialGradient>"
    "</defs>"

    "<!-- glow background -->"
    "<circle cx='50' cy='40' r='35' fill='url(#glow)'/>"

    "<!-- candle body -->"
    "<rect x='45' y='40' width='10' height='40' rx='3' fill='#f5e6c8'/>"

    "<!-- flame -->"
    "<path d='M50 10 C45 20, 40 28, 50 38 C60 28, 55 20, 50 10 Z' fill='#ffb300'/>"

    "</svg>";

/* ── HTML PAGE ───────────────────────────────────────────── */

static const char INDEX_HTML[] =
    "<!DOCTYPE html>"
    "<html lang='en'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
    "<meta name='theme-color' content='#1a0e00'>"
    "<title>Candle</title>"
    "<link rel='icon' href='/favicon.svg' type='image/svg+xml'>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{"
    "font-family:system-ui,sans-serif;"
    "background:#1a0e00;"
    "color:#f5e6c8;"
    "min-height:100vh;"
    "display:flex;flex-direction:column;align-items:center;"
    "padding:env(safe-area-inset-top,1.5rem) 1.2rem 5rem;"
    "background-image:radial-gradient(ellipse at 50% 0%,#3d1f00 0%,#1a0e00 70%);"
    "}"
    "h1{"
    "font-family:Georgia,serif;"
    "font-weight:300;font-size:2rem;"
    "letter-spacing:.15em;text-transform:uppercase;"
    "color:#f0c96e;margin-bottom:.25rem;"
    "text-shadow:0 0 40px rgba(240,180,80,.5);"
    "margin-top:1.5rem;"
    "}"
    ".subtitle{font-size:.7rem;letter-spacing:.22em;color:#6a5030;margin-bottom:1.8rem;text-transform:uppercase}"

    /* Connection badge */
    ".conn-badge{"
    "display:inline-flex;align-items:center;gap:.45rem;"
    "padding:.35rem 1rem;border-radius:99px;margin-bottom:1.6rem;"
    "font-size:.72rem;letter-spacing:.08em;border:1px solid;transition:all .5s;"
    "}"
    ".conn-badge.paired{background:rgba(80,200,80,.08);border-color:rgba(80,200,80,.3);color:#80d860}"
    ".conn-badge.unpaired{background:rgba(255,100,60,.07);border-color:rgba(255,100,60,.2);color:#c07050}"
    ".cbdot{width:6px;height:6px;border-radius:50%;transition:background .5s}"
    ".conn-badge.paired .cbdot{background:#60d840;box-shadow:0 0 6px #40b820}"
    ".conn-badge.unpaired .cbdot{background:#904030}"

    /* Status pill */
    ".status-pill{"
    "display:inline-flex;align-items:center;gap:.5rem;"
    "padding:.4rem 1.1rem;border-radius:99px;"
    "font-size:.8rem;letter-spacing:.08em;"
    "border:1px solid;margin-bottom:1.8rem;transition:all .4s;"
    "}"
    ".status-pill.on{background:rgba(240,180,60,.12);border-color:rgba(240,180,60,.4);color:#f0c060}"
    ".status-pill.off{background:rgba(255,255,255,.04);border-color:rgba(255,255,255,.08);color:#5a4830}"
    ".dot{width:7px;height:7px;border-radius:50%;transition:background .4s}"
    ".status-pill.on .dot{background:#f0c060;box-shadow:0 0 8px #f0a020}"
    ".status-pill.off .dot{background:#3a2a18}"

    /* Toggle button */
    ".toggle-btn{"
    "width:90px;height:90px;border-radius:50%;"
    "border:2px solid rgba(240,160,40,.25);"
    "background:transparent;color:#f0c060;font-size:2rem;"
    "cursor:pointer;transition:all .3s;"
    "display:flex;align-items:center;justify-content:center;"
    "margin-bottom:2rem;"
    "}"
    ".toggle-btn.on{"
    "background:rgba(240,160,40,.15);"
    "border-color:#f0a020;"
    "box-shadow:0 0 35px rgba(240,140,20,.3),0 0 70px rgba(240,140,20,.1);"
    "}"

    /* Controls */
    ".controls{width:100%;max-width:340px;display:flex;flex-direction:column;gap:1.4rem;margin-bottom:2rem}"
    ".ctrl-row label{"
    "display:flex;justify-content:space-between;align-items:baseline;"
    "font-size:.68rem;letter-spacing:.14em;text-transform:uppercase;"
    "color:#6a5030;margin-bottom:.55rem;"
    "}"
    ".ctrl-row label span{color:#b89050;font-size:.82rem;letter-spacing:0;font-variant-numeric:tabular-nums}"
    "input[type=range]{"
    "-webkit-appearance:none;width:100%;height:4px;"
    "border-radius:2px;outline:none;cursor:pointer;transition:opacity .2s;"
    "}"
    "input[type=range]:active{opacity:.85}"
    "input[type=range].bri{background:linear-gradient(to right,#2a1800,#f0c060)}"
    "input[type=range].ct{background:linear-gradient(to right,#ff8020,#ffe0c0,#c8d8ff)}"
    "input[type=range]::-webkit-slider-thumb{"
    "-webkit-appearance:none;width:20px;height:20px;border-radius:50%;"
    "background:#f5e0b8;border:2px solid #e09020;"
    "box-shadow:0 0 6px rgba(224,140,20,.5);cursor:grab;"
    "}"

    /* Favourites */
    ".section{width:100%;max-width:340px;margin-bottom:1.5rem}"
    ".section-title{"
    "font-size:.68rem;letter-spacing:.18em;text-transform:uppercase;"
    "color:#5a4020;margin-bottom:.8rem;"
    "padding-bottom:.4rem;border-bottom:1px solid rgba(255,255,255,.05);"
    "}"
    ".fav-list{display:flex;flex-direction:column;gap:.5rem;margin-bottom:1rem}"
    ".fav-item{"
    "display:flex;align-items:center;gap:.65rem;"
    "padding:.7rem .9rem;border-radius:9px;"
    "background:rgba(255,255,255,.025);border:1px solid rgba(255,255,255,.06);"
    "cursor:pointer;transition:background .15s;-webkit-tap-highlight-color:transparent;"
    "}"
    ".fav-item:active{background:rgba(240,160,40,.1)}"
    ".fav-preview{width:24px;height:24px;border-radius:50%;flex-shrink:0;border:1px solid rgba(255,255,255,.08)}"
    ".fav-info{flex:1;min-width:0}"
    ".fav-name{font-size:.88rem;color:#c8a860;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
    ".fav-sub{font-size:.68rem;color:#5a4020;margin-top:.1rem}"
    ".fav-del{background:none;border:none;color:#3a2010;font-size:1rem;cursor:pointer;padding:0 .3rem;flex-shrink:0;line-height:1}"
    ".fav-del:active{color:#c05030}"
    ".empty-favs{font-size:.8rem;color:#4a3020;text-align:center;padding:1rem 0;font-style:italic}"

    /* Add fav */
    ".add-fav{background:rgba(255,255,255,.02);border:1px solid rgba(255,255,255,.05);border-radius:9px;padding:.9rem}"
    ".add-fav input[type=text]{"
    "width:100%;background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.08);"
    "border-radius:6px;padding:.55rem .75rem;color:#f5e6c8;font-size:.88rem;"
    "outline:none;margin-bottom:.6rem;font-family:system-ui,sans-serif;"
    "-webkit-appearance:none;"
    "}"
    ".add-fav input[type=text]::placeholder{color:#3a2810}"
    ".add-fav input[type=text]:focus{border-color:rgba(240,160,40,.35)}"
    ".btn-save{"
    "width:100%;padding:.6rem;"
    "border:1px solid rgba(240,160,40,.35);background:rgba(240,160,40,.08);"
    "color:#d4a840;border-radius:6px;font-size:.82rem;letter-spacing:.05em;"
    "cursor:pointer;font-family:system-ui,sans-serif;"
    "-webkit-tap-highlight-color:transparent;"
    "}"
    ".btn-save:active{background:rgba(240,160,40,.18)}"

    /* Pair section */
    ".pair-section{width:100%;max-width:340px;margin-top:.5rem}"
    ".pair-row{display:flex;align-items:center;gap:.75rem}"
    ".pair-btn{"
    "flex:1;background:none;"
    "border:1px solid rgba(255,255,255,.07);color:#4a3820;"
    "padding:.55rem 1rem;border-radius:99px;"
    "font-size:.7rem;letter-spacing:.1em;text-transform:uppercase;"
    "cursor:pointer;font-family:system-ui,sans-serif;"
    "-webkit-tap-highlight-color:transparent;transition:all .2s;"
    "}"
    ".pair-btn:active,.pair-btn.active{border-color:rgba(240,160,40,.4);color:#a08040;background:rgba(240,160,40,.06)}"
    ".pair-hint{font-size:.65rem;color:#3a2410;flex:1;line-height:1.4}"

    /* Toast */
    ".toast{"
    "position:fixed;bottom:2rem;left:50%;transform:translateX(-50%);"
    "background:rgba(30,18,5,.96);border:1px solid rgba(240,160,40,.25);"
    "color:#d4a840;padding:.5rem 1.1rem;border-radius:99px;"
    "font-size:.78rem;pointer-events:none;white-space:nowrap;"
    "opacity:0;transition:opacity .25s;z-index:100;"
    "}"
    ".toast.show{opacity:1}"
    "</style></head><body>"

    "<h1>&#x1F56F;</h1>"
    "<p class='subtitle'>Candle</p>"

    /* Connection status */
    "<div class='conn-badge unpaired' id='connBadge'>"
    "<span class='cbdot'></span>"
    "<span id='connTxt'>Not paired</span>"
    "</div>"

    /* On/Off status */
    "<div class='status-pill off' id='pill'>"
    "<span class='dot'></span><span id='pillTxt'>OFF</span>"
    "</div>"

    /* Toggle */
    "<button class='toggle-btn' id='toggleBtn' onclick='toggle()'>&#9711;</button>"

    /* Sliders */
    "<div class='controls'>"
    "<div class='ctrl-row'>"
    "<label>Brightness <span id='briVal'>70%</span></label>"
    "<input type='range' class='bri' id='briSlider' min='1' max='100' value='70'"
    " oninput='onBri(this.value)' onchange='sendBri(this.value)'>"
    "</div>"
    "<div class='ctrl-row'>"
    "<label>Warmth <span id='ctVal'>3000 K</span></label>"
    "<input type='range' class='ct' id='ctSlider' min='2200' max='6500' step='50' value='3000'"
    " oninput='onCt(this.value)' onchange='sendCt(this.value)'>"
    "</div>"
    "</div>"

    /* Favourites */
    "<div class='section'>"
    "<div class='section-title'>Favourites</div>"
    "<div class='fav-list' id='favList'><p class='empty-favs'>No favourites yet</p></div>"
    "<div class='add-fav'>"
    "<input type='text' id='favName' placeholder='Name  (saves current brightness &amp; warmth)'>"
    "<button class='btn-save' onclick='saveFav()'>&#x2713; Save current settings</button>"
    "</div>"
    "</div>"

    /* Pair */
    "<div class='pair-section'>"
    "<div class='section-title'>Zigbee pairing</div>"
    "<div class='pair-row'>"
    "<button class='pair-btn' id='pairBtn' onclick='pair()'>&#x25CE; Touchlink pair</button>"
    "<p class='pair-hint'>Reset lamp (5&times; power cycle), hold ESP &lt;10 cm, then tap</p>"
    "</div>"
    "</div>"

    "<div class='toast' id='toast'></div>"

    "<script>"
    "var state={on:false,brightness:70,ct_kelvin:3000,paired:false};"
    "var briTimer=null,ctTimer=null;"

    /* Kelvin → preview colour */
    "function kCol(k){"
    "var t=k/100,r,g,b;"
    "if(t<=66){r=255;g=Math.max(0,Math.min(255,99.47*Math.log(t)-161.12));b=(t<=19)?0:Math.max(0,Math.min(255,138.52*Math.log(t-10)-305.04));}"
    "else{r=Math.max(0,Math.min(255,329.7*Math.pow(t-60,-.133)));g=Math.max(0,Math.min(255,288.1*Math.pow(t-60,-.076)));b=255;}"
    "return [Math.round(r),Math.round(g),Math.round(b)];"
    "}"

    "function toast(m,dur){"
    "var t=document.getElementById('toast');t.textContent=m;t.classList.add('show');"
    "setTimeout(()=>t.classList.remove('show'),dur||2000);"
    "}"

    "async function api(url,opts){"
    "try{var r=await fetch(url,opts||{});"
    "if(!r.ok)toast('HTTP '+r.status);"
    "return r;"
    "}catch(e){toast('Network error');return null;}"
    "}"

    "function render(){"
    /* connection badge */
    "var cb=document.getElementById('connBadge'),ct2=document.getElementById('connTxt');"
    "if(state.paired){cb.className='conn-badge paired';ct2.textContent='Lamp connected';}"
    "else{cb.className='conn-badge unpaired';ct2.textContent='Not paired';}"
    /* on/off pill */
    "var pill=document.getElementById('pill'),ptxt=document.getElementById('pillTxt');"
    "var btn=document.getElementById('toggleBtn');"
    "if(state.on){"
    "pill.className='status-pill on';ptxt.textContent='ON';"
    "btn.classList.add('on');btn.innerHTML='&#9728;';"
    "}else{"
    "pill.className='status-pill off';ptxt.textContent='OFF';"
    "btn.classList.remove('on');btn.innerHTML='&#9711;';"
    "}"
    /* sliders */
    "document.getElementById('briSlider').value=state.brightness;"
    "document.getElementById('briVal').textContent=state.brightness+'%';"
    "document.getElementById('ctSlider').value=state.ct_kelvin;"
    "document.getElementById('ctVal').textContent=state.ct_kelvin+' K';"
    "}"

    "async function fetchState(){"
    "var r=await api('/api/state');"
    "if(r){var d=await r.json();state=d;render();}"
    "}"

    "async function toggle(){"
    "if(!state.paired){toast('Pair the lamp first');return;}"
    "var on=!state.on;state.on=on;render();"
    "await api(on?'/api/on':'/api/off',{method:'POST'});"
    "}"

    /* Live brightness */
    "function onBri(v){"
    "v=parseInt(v);"
    "document.getElementById('briVal').textContent=v+'%';"
    "clearTimeout(briTimer);"
    "briTimer=setTimeout(()=>{"
    "if(!state.paired)return;"
    "state.brightness=v;if(!state.on){state.on=true;render();}"
    "api('/api/set',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({brightness:v})});"
    "},50);"
    "}"
    "function sendBri(v){"
    "v=parseInt(v);state.brightness=v;"
    "if(!state.paired)return;"
    "if(!state.on){state.on=true;render();}"
    "api('/api/set',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({brightness:v})});"
    "}"

    /* Live color temp */
    "function onCt(v){"
    "v=parseInt(v);"
    "document.getElementById('ctVal').textContent=v+' K';"
    "clearTimeout(ctTimer);"
    "ctTimer=setTimeout(()=>{"
    "if(!state.paired)return;"
    "state.ct_kelvin=v;if(!state.on){state.on=true;render();}"
    "api('/api/set',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ct_kelvin:v})});"
    "},50);"
    "}"
    "function sendCt(v){"
    "v=parseInt(v);state.ct_kelvin=v;"
    "if(!state.paired)return;"
    "if(!state.on){state.on=true;render();}"
    "api('/api/set',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ct_kelvin:v})});"
    "}"

    /* Favourites */
    "async function loadFavs(){"
    "var r=await api('/api/favorites');if(!r)return;"
    "var favs=await r.json();"
    "var list=document.getElementById('favList');"
    "if(!favs.length){list.innerHTML=\"<p class='empty-favs'>No favourites yet</p>\";return;}"
    "list.innerHTML='';"
    "favs.forEach(function(f){"
    "var rgb=kCol(f.ct_kelvin);var bri=f.brightness/100;"
    "var col='rgb('+Math.round(rgb[0]*bri)+','+Math.round(rgb[1]*bri)+','+Math.round(rgb[2]*bri)+')';"
    "var d=document.createElement('div');d.className='fav-item';"
    "d.innerHTML='<div class=\"fav-preview\" style=\"background:'+col+'\"></div>'"
    "+'<div class=\"fav-info\"><div class=\"fav-name\">'+f.name+'</div>'"
    "+'<div class=\"fav-sub\">'+f.brightness+'% &middot; '+f.ct_kelvin+' K</div></div>'"
    "+'<button class=\"fav-del\" onclick=\"delFav('+f.id+',event)\">&#x2715;</button>';"
    "d.onclick=function(){applyFav(f.id);};"
    "list.appendChild(d);"
    "});"
    "}"

    "async function applyFav(id){"
    "if(!state.paired){toast('Pair the lamp first');return;}"
    "await api('/api/favorites/'+id,{method:'PUT'});"
    "setTimeout(fetchState,400);toast('Applied');"
    "}"

    "async function delFav(id,ev){"
    "ev.stopPropagation();"
    "await api('/api/favorites/'+id,{method:'DELETE'});"
    "loadFavs();toast('Deleted');"
    "}"

    "async function saveFav(){"
    "var name=document.getElementById('favName').value.trim();"
    "if(!name){toast('Enter a name');return;}"
    "await api('/api/favorites',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({name:name,brightness:state.brightness,ct_kelvin:state.ct_kelvin})});"
    "document.getElementById('favName').value='';"
    "loadFavs();toast('Saved');"
    "}"

    /* Pair */
    "async function pair(){"
    "var btn=document.getElementById('pairBtn');"
    "btn.classList.add('active');btn.textContent='Scanning...';"
    "toast('Touchlink started — hold ESP near lamp',4000);"
    "await api('/cmd?pair');"
    "setTimeout(()=>{"
    "btn.classList.remove('active');btn.innerHTML='&#x25CE; Touchlink pair';"
    "fetchState();"
    "},5000);"
    "}"

    "fetchState();loadFavs();"
    "setInterval(fetchState,4000);"
    "</script></body></html>";

/* ── Handlers ─────────────────────────────────────────────────────── */

static esp_err_t h_root(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
  return ESP_OK;
}

static esp_err_t h_state_get(httpd_req_t *req)
{
  device_state_t st = get_current_state();
  bool paired = get_lamp_paired();
  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"on\":%s,\"brightness\":%d,\"ct_kelvin\":%d,\"paired\":%s}",
           st.on ? "true" : "false",
           st.brightness,
           st.ct_kelvin,
           paired ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, strlen(buf));
  return ESP_OK;
}

static esp_err_t h_on(httpd_req_t *req)
{
  set_on_off(true);
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t h_off(httpd_req_t *req)
{
  set_on_off(false);
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t h_favicon(httpd_req_t *req)
{
  httpd_resp_set_type(req, "image/svg+xml");
  httpd_resp_send(req, FAVICON_SVG, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static esp_err_t h_set(httpd_req_t *req)
{
  char buf[256];
  int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (len <= 0)
    return ESP_FAIL;
  buf[len] = '\0';

  ESP_LOGI(TAG, "/api/set: %s", buf);

  cJSON *json = cJSON_Parse(buf);
  if (json)
  {
    /* Handle optional on/off inside set */
    cJSON *on_field = cJSON_GetObjectItem(json, "on");
    if (on_field && cJSON_IsBool(on_field))
    {
      set_on_off(cJSON_IsTrue(on_field));
    }

    cJSON *bri = cJSON_GetObjectItem(json, "brightness");
    if (bri)
    {
      int v = cJSON_IsNumber(bri) ? bri->valueint : atoi(bri->valuestring ? bri->valuestring : "0");
      if (v >= 1 && v <= 100)
        set_brightness((uint8_t)v);
    }

    cJSON *ct = cJSON_GetObjectItem(json, "ct_kelvin");
    if (ct)
    {
      int v = cJSON_IsNumber(ct) ? ct->valueint : atoi(ct->valuestring ? ct->valuestring : "0");
      if (v >= 2200 && v <= 6500)
        set_color_temp((uint16_t)v);
    }

    cJSON_Delete(json);
  }
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t h_fav_get(httpd_req_t *req)
{
  cJSON *arr = cJSON_CreateArray();
  for (uint8_t i = 0; i < MAX_FAVORITES; i++)
  {
    favorite_t fav;
    if (favorites_get(i, &fav))
    {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddNumberToObject(o, "id", i);
      cJSON_AddStringToObject(o, "name", fav.name);
      cJSON_AddNumberToObject(o, "brightness", fav.brightness);
      cJSON_AddNumberToObject(o, "ct_kelvin", fav.ct_kelvin);
      cJSON_AddItemToArray(arr, o);
    }
  }
  char *resp = cJSON_PrintUnformatted(arr);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, strlen(resp));
  cJSON_Delete(arr);
  free(resp);
  return ESP_OK;
}

static esp_err_t h_fav_post(httpd_req_t *req)
{
  char buf[256];
  int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (len <= 0)
    return ESP_FAIL;
  buf[len] = '\0';

  cJSON *json = cJSON_Parse(buf);
  if (json)
  {
    cJSON *name = cJSON_GetObjectItem(json, "name");
    cJSON *bri = cJSON_GetObjectItem(json, "brightness");
    cJSON *ct = cJSON_GetObjectItem(json, "ct_kelvin");
    if (name && cJSON_IsString(name) && bri)
    {
      favorite_t fav = {0};
      strlcpy(fav.name, name->valuestring, sizeof(fav.name));
      fav.brightness = (uint8_t)(cJSON_IsNumber(bri) ? bri->valueint : atoi(bri->valuestring));
      fav.ct_kelvin = (uint16_t)(ct && cJSON_IsNumber(ct) ? ct->valueint : 3000);
      favorites_add(&fav);
    }
    cJSON_Delete(json);
  }
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t h_fav_put(httpd_req_t *req)
{
  const char *last = strrchr(req->uri, '/');
  if (last)
    apply_favorite((uint8_t)atoi(last + 1));
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t h_fav_del(httpd_req_t *req)
{
  const char *last = strrchr(req->uri, '/');
  if (last)
    favorites_delete((uint8_t)atoi(last + 1));
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

/* /cmd — Android home screen shortcuts */ static esp_err_t h_cmd(httpd_req_t *req)
{
  char query[64] = {0};
  httpd_req_get_url_query_str(req, query, sizeof(query));

  if (strncmp(query, "on", 2) == 0)
  {
    set_on_off(true);
  }
  else if (strncmp(query, "off", 3) == 0)
  {
    set_on_off(false);
  }
  else if (strncmp(query, "pair", 4) == 0)
  {
    start_touchlink_pairing();
  }
  else
  {
    char *fv = strstr(query, "fav=");
    if (fv)
      apply_favorite((uint8_t)atoi(fv + 4));
  }

  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

/* ── Start server ─────────────────────────────────────────────────── */
void start_web_server(void)
{
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  cfg.max_uri_handlers = 16;
  cfg.lru_purge_enable = true;

  if (httpd_start(&server, &cfg) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return;
  }

  const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = h_root},
      {.uri = "/favicon.svg", .method = HTTP_GET, .handler = h_favicon},
      {.uri = "/api/state", .method = HTTP_GET, .handler = h_state_get},
      {.uri = "/api/on", .method = HTTP_POST, .handler = h_on},
      {.uri = "/api/off", .method = HTTP_POST, .handler = h_off},
      {.uri = "/api/set", .method = HTTP_POST, .handler = h_set},
      {.uri = "/api/favorites", .method = HTTP_GET, .handler = h_fav_get},
      {.uri = "/api/favorites", .method = HTTP_POST, .handler = h_fav_post},
      {.uri = "/api/favorites/*", .method = HTTP_PUT, .handler = h_fav_put},
      {.uri = "/api/favorites/*", .method = HTTP_DELETE, .handler = h_fav_del},
      {.uri = "/cmd", .method = HTTP_GET, .handler = h_cmd},
  };
  for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
  {
    httpd_register_uri_handler(server, &routes[i]);
  }
  ESP_LOGI(TAG, "HTTP server started");
}