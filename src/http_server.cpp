#include "http_server.h"

#include <ESP8266WiFi.h>
#include <ArduinoJson.h>

#include "config.h"
#include "storage.h"
#include "rs485.h"
#include "shelly.h"
#include "wifi_mgr.h"
#include "mqtt_mgr.h"
#include "telnet_log.h"
#include "help_page.h"

ESP8266WebServer server(80);

// Wenn ein Neustart nötig ist (z.B. nach dem Speichern einer neuen
// Konfiguration), darf man nicht sofort ESP.restart() aufrufen: dann würde
// die laufende HTTP-Antwort ("OK, gespeichert...") nie beim Browser ankommen,
// weil die Verbindung mitten drin abreißt. Stattdessen wird hier nur ein
// Zeitpunkt in der (nahen) Zukunft gemerkt; httpServerLoop() prüft bei jedem
// Durchlauf, ob dieser Zeitpunkt erreicht ist, und startet erst dann neu --
// das gibt der Antwort genug Zeit, noch rauszugehen. Auch das ist wieder das
// millis()-Zeitstempel-Muster statt eines blockierenden delay().
static bool restartPending = false;
static unsigned long restartAtMillis = 0;

static void scheduleRestart(unsigned long delayMs) {
    restartPending = true;
    restartAtMillis = millis() + delayMs;
}

// ---------------------------------------------------------------------------
// Webinterface (Dark-Mode, mobil-optimiert, Auto-Refresh per fetch("/status"))
//
// PROGMEM: Dieser HTML/CSS/JS-Text ist mehrere KB groß. Ohne PROGMEM würde er
// beim Start komplett in den ohnehin knappen RAM des ESP8266 kopiert werden.
// Mit PROGMEM bleibt er im (viel größeren) Flash-Speicher liegen und wird nur
// bei Bedarf ausgelesen (server.send_P(...) statt server.send(...), das "_P"
// steht für "aus PROGMEM lesen").
// R"rawliteral( ... )rawliteral" ist ein "raw string literal": alles zwischen
// den Klammern wird 1:1 als Text übernommen, ohne dass z.B. Anführungszeichen
// im HTML extra escaped werden müssten.
// ---------------------------------------------------------------------------
static const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>Soyosource Regler</title>
<style>
:root{--bg:#f5f5f7;--fg:#1c1c1e;--card:#ffffff;--border:#d0d0d5;--accent:#0a84ff;--red:#ff3b30;--green:#34c759;--yellow:#ffcc00;--muted:#8a8a8e;}
@media (prefers-color-scheme: dark){:root{--bg:#000000;--fg:#f2f2f7;--card:#1c1c1e;--border:#3a3a3c;--muted:#98989d;}}
*{box-sizing:border-box;}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font-family:-apple-system,"Segoe UI",Roboto,sans-serif;}
h1{font-size:1.3em;margin:0 0 14px 0;transition:opacity .3s;}
h2{font-size:1.05em;margin:0 0 8px 0;}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:14px;margin-bottom:14px;}
.row{display:flex;justify-content:space-between;gap:10px;padding:7px 0;border-bottom:1px solid var(--border);}
.row:last-child{border-bottom:none;}
.row.blink{animation:blink 1s step-start infinite;}
@keyframes blink{50%{opacity:.25;}}
.btn{display:inline-block;padding:12px 18px;border-radius:10px;border:none;font-size:1em;margin:4px 6px 4px 0;min-height:44px;color:#fff;cursor:pointer;}
.btn-red{background:var(--red);}
.btn-green{background:var(--green);}
.btn-blue{background:var(--accent);}
.btn-muted{background:var(--muted);}
.warning{background:var(--yellow);color:#2c2c00;padding:10px 12px;border-radius:10px;margin-bottom:14px;display:none;font-weight:600;}
.rssi-bars{display:inline-flex;align-items:flex-end;gap:2px;height:18px;}
.rssi-bars span{width:4px;background:var(--muted);border-radius:1px;display:inline-block;}
.rssi-bars span.on{background:var(--green);}
fieldset{border:1px solid var(--border);border-radius:10px;margin-bottom:14px;padding:10px 12px 14px;}
legend{padding:0 6px;font-weight:600;}
label{display:block;margin:10px 0 3px;font-size:.85em;color:var(--muted);}
input[type=text],input[type=password],input[type=number],input[type=time],select{
width:100%;padding:10px;border-radius:8px;border:1px solid var(--border);background:var(--bg);color:var(--fg);font-size:1em;}
.checkbox-row{display:flex;align-items:center;gap:8px;margin:10px 0;}
.checkbox-row input{width:20px;height:20px;}
.hint{font-size:.82em;color:var(--muted);margin-top:4px;}
a{color:var(--accent);}
code{background:var(--bg);border:1px solid var(--border);border-radius:4px;padding:1px 5px;font-size:.9em;}
#staticIpFields,#jsonSection,#nightFields,#staticSection{display:none;}
.status-dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:var(--muted);margin-right:4px;vertical-align:middle;}
.status-dot.on{background:var(--green);}
.status-dot.off{background:var(--red);}
.status-item{display:inline-flex;align-items:center;margin-right:14px;font-size:.9em;}
.saved-ok{color:var(--green);font-size:.85em;margin-left:8px;}
.tabbar{display:flex;gap:6px;margin-bottom:14px;overflow-x:auto;}
.tab-btn{flex:1;padding:10px 8px;border-radius:10px;border:1px solid var(--border);background:var(--card);color:var(--fg);font-size:.95em;cursor:pointer;white-space:nowrap;}
.tab-btn.active{background:var(--accent);color:#fff;border-color:var(--accent);}
.tab-panel{display:none;}
.tab-panel.active{display:block;}
</style>
</head>
<body>

<h1>Soyosource Regler</h1>

<div id="apwarning" class="warning">AP-Setup-Modus aktiv &mdash; bitte WLAN unten konfigurieren und speichern.</div>
<div id="fallbackWarning" class="warning">Fallback aktiv &mdash; Messwertquelle antwortet nicht, Ausgabe auf Fallback-Watt.</div>

<div class="tabbar">
  <button type="button" class="tab-btn active" id="tabbtn-main" onclick="switchTab('main')">Hauptseite</button>
  <button type="button" class="tab-btn" id="tabbtn-device" onclick="switchTab('device')">Gerätekonfig</button>
  <button type="button" class="tab-btn" id="tabbtn-network" onclick="switchTab('network')">Netzwerkkonfig</button>
  <button type="button" class="tab-btn" id="tabbtn-help" onclick="switchTab('help')">Help</button>
</div>

<div class="tab-panel active" id="tab-main">

<div class="card">
  <h2>Status</h2>
  <div class="row">
    <span>Verbindungen</span>
    <span>
      <span class="status-item"><span class="status-dot" id="dotEsp"></span>ESP</span>
      <span class="status-item"><span class="status-dot" id="dotSoyo"></span>Soyo</span>
      <span class="status-item" id="dotSourceItem"><span class="status-dot" id="dotSource"></span>Messwertquelle</span>
      <span class="status-item"><span class="status-dot" id="dotMqtt"></span>MQTT</span>
    </span>
  </div>
  <div class="row"><span>Demand</span><span><b id="demand">-</b> W<span id="demandTotal"></span></span></div>
  <div class="row"><span>Netzwert</span><span id="netz">-</span></div>
  <div class="row" id="exportStatusRow" style="display:none;"><span>Einspeisung</span><span id="exportStatus">-</span></div>
  <div class="row"><span>Modus</span><span id="modeText">-</span></div>
  <div class="row"><span>Notaus</span><span id="notausState">-</span></div>
  <div class="row"><span>WLAN RSSI</span><span class="rssi-bars" id="rssiBars"></span></div>
  <div class="row"><span>Laufzeit</span><span id="uptime">-</span></div>
  <div class="row"><span>Firmware</span><span id="buildTime">-</span></div>
  <div class="row"><span>Version</span><span id="gitVersion">-</span></div>
  <div class="row"><span>Freier Heap</span><span><span id="heap">-</span> B</span></div>
  <div class="row"><span>Hauptschleife</span><span id="loopTiming">-</span></div>
  <div class="hint">Mittel / Maximum im letzten Messfenster (ca. 5s), Spitze seit Start; Laufzeit inklusive Wartezeiten, keine CPU-Auslastung.</div>
  <div class="row"><span>Messwert</span><span id="measurementAge">-</span></div>
  <div class="row blink" id="dcRow"><span>DC-Einspeisung</span><span id="dcState">-</span></div>
</div>

<div class="card" id="soyoStatus" style="display:none;">
  <h2>Soyo-Status (RS485)</h2>
  <div class="row"><span>Batteriespannung</span><span><span id="soyoBattV">-</span> V</span></div>
  <div class="row"><span>Batteriestrom</span><span><span id="soyoBattA">-</span> A</span></div>
  <div class="row"><span>AC-Spannung</span><span><span id="soyoAcV">-</span> V</span></div>
  <div class="row"><span>Frequenz</span><span><span id="soyoFreq">-</span> Hz</span></div>
  <div class="row"><span>Temperatur</span><span><span id="soyoTemp">-</span> &deg;C</span></div>
</div>

<div class="card">
  <h2>Steuerung</h2>
  <button class="btn btn-red" onclick="notaus(true)">Notaus</button>
  <button class="btn btn-green" onclick="notaus(false)">Notaus aufheben</button>
  <button class="btn btn-muted" onclick="restart()">Neustart</button>
</div>

</div>
<div class="tab-panel" id="tab-network">

<div class="card">
  <h2>Netzwerkkonfiguration</h2>
  <p class="hint">Änderungen hier brauchen einen Neustart, um wirksam zu werden (WLAN/MQTT-Verbindung wird neu aufgebaut).</p>

  <fieldset>
    <legend>WLAN</legend>
    <label>SSID</label>
    <input type="text" id="wifi_ssid">
    <label>Passwort</label>
    <input type="password" id="wifi_pass">
    <label>Fallback-SSID (optional)</label>
    <input type="text" id="wifi_ssid2">
    <label>Fallback-Passwort</label>
    <input type="password" id="wifi_pass2">
    <p class="hint">Wird erst probiert, wenn das primäre WLAN oben eine Weile nicht erreichbar ist (z.B. Handy-Hotspot als Notlösung). Leer lassen = kein Fallback. Läuft das Gerät gerade auf dem Fallback, wird alle paar Minuten im Hintergrund geprüft, ob das primäre WLAN wieder da ist, und automatisch zurückgewechselt. Die feste IP unten gilt nur für das primäre WLAN, das Fallback-Netz bekommt immer eine Adresse per DHCP.</p>
    <div class="checkbox-row"><input type="checkbox" id="wifi_static" onchange="onStaticToggle()"><label style="margin:0;">Statische IP</label></div>
    <div id="staticIpFields">
      <label>IP-Adresse</label>
      <input type="text" id="wifi_ip" placeholder="192.168.1.200">
      <label>Gateway</label>
      <input type="text" id="wifi_gw" placeholder="192.168.1.1">
      <label>Subnetzmaske</label>
      <input type="text" id="wifi_mask" placeholder="255.255.255.0">
    </div>
    <div class="checkbox-row"><input type="checkbox" id="wifi_11n"><label style="margin:0;">802.11n erzwingen</label></div>
    <button type="button" class="btn btn-blue" onclick="saveSection('wlan')">WLAN speichern</button>
    <span class="saved-ok" id="saved-wlan"></span>
  </fieldset>

  <fieldset>
    <legend>MQTT</legend>
    <div class="checkbox-row"><input type="checkbox" id="mqtt_enabled"><label style="margin:0;">MQTT aktiv</label></div>
    <label>Broker</label>
    <input type="text" id="mqtt_broker">
    <label>Port</label>
    <input type="number" id="mqtt_port" value="1883">
    <label>Benutzer</label>
    <input type="text" id="mqtt_user">
    <label>Passwort</label>
    <input type="password" id="mqtt_pass">
    <label>Sub-Topic (Messwert)</label>
    <input type="text" id="mqtt_sub_topic">
    <label>Messwert-Eingabeformat (MqttSub)</label>
    <select id="mqtt_input_format">
      <option value="0">Zahl (W)</option>
      <option value="1">Shelly-JSON (total_act_power)</option>
    </select>
    <p class="hint">Shelly-JSON nutzt ausschließlich die Gesamtwirkleistung total_act_power, unabhängig von der Phasenauswahl. Topic: &lt;Shelly-ID&gt;/status/em:0. Null und fehlende Felder sind keine Messwerte; 0 W und negative Werte sind gültig.</p>
    <label>Pub-Topic (Status)</label>
    <input type="text" id="mqtt_pub_topic">
    <p class="hint">MQTT verbindet sich mit dem Broker und veröffentlicht dort Statuswerte. Der MQTT-Statuspunkt auf der Hauptseite zeigt ausschließlich, ob aktuell eine Broker-Verbindung besteht.</p>
    <button type="button" class="btn btn-blue" onclick="saveSection('mqtt')">MQTT speichern</button>
    <span class="saved-ok" id="saved-mqtt"></span>
  </fieldset>
</div>

</div>
<div class="tab-panel" id="tab-device">

<div class="card">
  <h2>Gerätekonfiguration</h2>
  <p class="hint">Änderungen hier wirken sofort, ohne Neustart (Ausnahme: OTA-Passwort).</p>

  <fieldset>
    <legend>Betriebsmodus</legend>
    <label>Modus</label>
    <select id="mode" onchange="onModeChange()">
      <option value="0">Static (konstante Leistung)</option>
      <option value="1">HttpInterface (/L1L2L3Auto?Value=x)</option>
      <option value="2">MqttSub</option>
      <option value="3">Shelly Gen1</option>
      <option value="4">Shelly Gen2 Pro</option>
      <option value="5">JSON HTTP Client</option>
    </select>

    <div id="staticSection">
      <label>Statische Leistung, Gesamt (W)</label>
      <input type="number" id="static_watt">
      <div class="hint">Gilt für die Summe aller angeschlossenen Soyos (siehe
      "Anzahl der Soyos am Bus" unten) -- die Firmware teilt den Wert selbst
      passend auf, nicht pro Gerät einzeln eintragen.</div>
    </div>

    <label>Shelly IP</label>
    <input type="text" id="shelly_ip" placeholder="192.168.1.50">
    <label>Phasen (Gen1 &amp; Gen2, beliebig kombinierbar -- alle drei angehakt = Gesamtleistung)</label>
    <div class="checkbox-row"><input type="checkbox" id="shelly_l1"><label style="margin:0;">L1</label></div>
    <div class="checkbox-row"><input type="checkbox" id="shelly_l2"><label style="margin:0;">L2</label></div>
    <div class="checkbox-row"><input type="checkbox" id="shelly_l3"><label style="margin:0;">L3</label></div>

    <fieldset>
      <legend>Optionaler Einspeisezähler (Shelly Gen2 Pro)</legend>
      <label>Shelly-IP des Einspeisezählers</label>
      <input type="text" id="shelly_export_ip" placeholder="192.168.1.51">
      <label>Phasen des Einspeisezählers (beliebig kombinierbar)</label>
      <div class="checkbox-row"><input type="checkbox" id="shelly_export_l1"><label style="margin:0;">L1</label></div>
      <div class="checkbox-row"><input type="checkbox" id="shelly_export_l2"><label style="margin:0;">L2</label></div>
      <div class="checkbox-row"><input type="checkbox" id="shelly_export_l3"><label style="margin:0;">L3</label></div>
      <p class="hint">Für die optionale Messung der Einspeisung über Shelly Gen2 Pro (HTTP-RPC). Der Zähler wird mit demselben Poll-Intervall wie der Verbrauchszähler abgefragt. Seine Werte dienen nur dem Monitoring; Messfehler beeinflussen weder Regelung noch Fallback.</p>
      <button type="button" class="btn btn-blue" onclick="saveSection('einspeisezaehler')">Einspeisezähler speichern</button>
      <span class="saved-ok" id="saved-einspeisezaehler"></span>
    </fieldset>

    <div id="jsonSection">
      <label>JSON-URL</label>
      <input type="text" id="json_url" placeholder="http://host/status">
      <label>JSON-Pfad (Punkt-separiert)</label>
      <input type="text" id="json_path" placeholder="StatusSNS.SML.DJ_TPWRCURR">
    </div>
    <button type="button" class="btn btn-blue" onclick="saveSection('mode')">Betriebsmodus speichern</button>
    <span class="saved-ok" id="saved-mode"></span>
  </fieldset>

  <fieldset>
    <legend>Regelung</legend>
    <label>Max. Leistung pro Inverter (W)</label>
    <input type="number" id="max_power">
    <div class="hint">Sicherheitslimit pro einzelnem Soyo (z.B. dessen
    Typenschild-Maximalwert), nicht die Summe. Bei ungleich starken
    Invertern am selben Bus den Wert der stärksten Geräte eintragen -- ein
    schwächeres Gerät begrenzt sich selbst.</div>
    <label>Anzahl der Soyos am Bus (1-12)</label>
    <input type="number" id="soyo_count" min="1" max="12">
    <label>Zähler-Kalibrierung (W)</label>
    <input type="number" id="calibration_offset_w">
    <div class="hint">Wird zu jedem Rohmesswert addiert, bevor Toleranz/Regelung
    ihn sehen -- gleicht einen systematisch falsch kalibrierten Stromzähler aus
    (z.B. zeigt er immer 5W zu viel Bezug an). Kein Richtungs-Bias, nur
    Sensorkorrektur.</div>
    <label>Fallback-Watt, Gesamt</label>
    <input type="number" id="fallback_watt">
    <div class="hint">Gilt für die Summe aller angeschlossenen Soyos, nicht
    pro Gerät.</div>
    <label>Toleranz Bezug (W)</label>
    <input type="number" id="tolerance_import_w" min="5" max="50">
    <label>Toleranz Einspeisung (W)</label>
    <input type="number" id="tolerance_export_w" min="5" max="50">
    <div class="hint">Je 5-50W, Standard je 10W. Solange der Messwert nicht
    mindestens in die jeweilige Richtung diese Grenze erreicht, wird nicht
    nachgeregelt -- verhindert, dass reines Messrauschen bei jedem Zyklus eine
    Korrektur auslöst. Unabhängig voneinander einstellbar: wer möglichst
    strikte Nulleinspeisung will, setzt "Toleranz Einspeisung" eng und
    "Toleranz Bezug" weiter; wer beide Richtungen gleich behandeln will
    (z.B. bei angemeldeter, unkritischer minimaler Einspeisung), setzt beide
    gleich.</div>
    <label>Regelkorrektur-Dämpfung (%)</label>
    <input type="number" id="correction_gain_percent" min="10" max="100">
    <div class="hint">10-100%, Standard 50%. Wie stark eine einzelne Korrektur
    pro Regeldurchlauf ausfällt (100% = alte, ungedämpfte Vollkorrektur). Ein
    kleinerer Wert reagiert langsamer, verhindert aber ein Aufschaukeln
    (Sägezahn-Schwingen), falls die tatsächliche Wirkung einer Korrektur dem
    Messwert am Zähler etwas hinterherhinkt.</div>
    <label>Poll-Intervall Shelly/JSON (ms)</label>
    <input type="number" id="poll_interval_ms" min="400" max="2000" step="50">
    <div class="hint">400-2000ms. Kleinere Werte reagieren schneller, blockieren
    den ESP aber länger pro Abfrage (Webinterface kann dabei träge wirken).</div>
    <label>RS485-Sendeintervall (ms)</label>
    <input type="number" id="rs485_send_interval_ms" min="1000" max="3000" step="100">
    <div class="hint">1000-3000ms, Standard 1100ms. Wie oft ein neuer Sollwert an
    den Soyo gesendet wird. Ein Referenzcontroller mit identischer Hardware
    läuft empirisch bestätigt stabil bei ~2000ms -- dieser Bereich eignet sich
    zum eigenen Testen, ohne den vom Hersteller nicht dokumentierten sicheren
    Rahmen zu verlassen.</div>
    <button type="button" class="btn btn-blue" onclick="saveSection('regelung')">Regelung speichern</button>
    <span class="saved-ok" id="saved-regelung"></span>
  </fieldset>

  <fieldset>
    <legend>Nachtmodus</legend>
    <p class="hint">Begrenzt im eingestellten Zeitfenster die maximale Leistung pro Inverter. Es wird keine feste Leistung eingespeist; die normale Regelung läuft weiter.</p>
    <div class="checkbox-row"><input type="checkbox" id="night_mode_enabled" onchange="onNightToggle()"><label style="margin:0;">Aktiv</label></div>
    <div id="nightFields">
      <label>Von</label>
      <input type="time" id="night_start">
      <label>Bis</label>
      <input type="time" id="night_end">
      <label>Max. Leistung nachts pro Inverter (W)</label>
      <input type="number" id="night_max_power">
    </div>
    <button type="button" class="btn btn-blue" onclick="saveSection('nacht')">Nachtmodus speichern</button>
    <span class="saved-ok" id="saved-nacht"></span>
  </fieldset>

  <fieldset>
    <legend>OTA</legend>
    <label>OTA-Passwort</label>
    <input type="password" id="ota_pass">
    <div class="hint">Schützt den Firmware-Upload über die OTA-Update-Seite im WLAN. Eine Passwortänderung wird nach einem Neustart wirksam.</div>
    <button type="button" class="btn btn-blue" onclick="saveSection('ota')">OTA speichern</button>
    <span class="saved-ok" id="saved-ota"></span>
  </fieldset>
</div>

<div class="card">
  <h2>Konfiguration sichern</h2>
  <a class="btn btn-muted" href="/config" download="config.json">Config Download</a>
  <br>
  <label>Config-Restore (JSON-Datei)</label>
  <input type="file" id="uploadFile" accept="application/json">
  <button class="btn btn-muted" onclick="uploadConfig()">Config Upload</button>
</div>

<div class="card">
  <a href="/update">Firmware-Update (OTA)</a>
</div>

</div>
<div class="tab-panel" id="tab-help">
  <p class="hint">Lädt...</p>
</div>

<script>
const MODE_NAMES = ['Static','HttpInterface','MqttSub','Shelly Gen1','Shelly Gen2 Pro','JSON HTTP Client'];

function pad(n){return n.toString().padStart(2,'0');}

function fmtUptime(sec){
  const d=Math.floor(sec/86400), h=Math.floor((sec%86400)/3600), m=Math.floor((sec%3600)/60);
  let s='';
  if(d>0) s+=d+'d ';
  s+=h+'h '+m+'m';
  return s;
}

function rssiBars(rssi){
  let bars=0;
  if(rssi>-55) bars=4; else if(rssi>-65) bars=3; else if(rssi>-75) bars=2; else if(rssi>-85) bars=1;
  let html='';
  for(let i=1;i<=4;i++){
    html+='<span class="'+(i<=bars?'on':'')+'" style="height:'+(i*4+4)+'px"></span>';
  }
  return html;
}

function refreshStatus(){
  fetch('/status').then(r=>r.json()).then(d=>{
    document.getElementById('dotEsp').classList.add('on');

    document.getElementById('demand').textContent=d.demand;
    // "Demand" ist der Sollwert, der identisch an jeden Soyo am Bus gesendet
    // wird (siehe rs485.cpp) -- bei mehr als einem Soyo ist die tatsächliche
    // Gesamteinspeisung also ein Vielfaches davon, nicht der angezeigte Wert
    // selbst. Nur anzeigen, wenn es überhaupt mehr als einen Soyo gibt, sonst
    // wäre "(x W gesamt)" nur eine Wiederholung derselben Zahl.
    document.getElementById('demandTotal').textContent=(d.soyo_count>1)?(' ('+(d.demand*d.soyo_count)+' W gesamt)'):'';
    document.getElementById('netz').textContent=
      'L1 '+fmtPower(d.verbrauch_l1)+' / L2 '+fmtPower(d.verbrauch_l2)+
      ' / L3 '+fmtPower(d.verbrauch_l3)+' / Gesamt '+fmtPower(d.netz);
    const exportParts=[];
    if(d.einspeisung_l1_enabled) exportParts.push('L1 '+fmtPower(d.einspeisung_l1));
    if(d.einspeisung_l2_enabled) exportParts.push('L2 '+fmtPower(d.einspeisung_l2));
    if(d.einspeisung_l3_enabled) exportParts.push('L3 '+fmtPower(d.einspeisung_l3));
    exportParts.push('Gesamt '+fmtPower(d.einspeisung));
    if(d.einspeisung_stale) exportParts.push('veraltet – vor '+d.einspeisung_age+'s aktualisiert');
    else if(d.einspeisung===null) exportParts.push('nicht verfügbar');
    document.getElementById('exportStatus').textContent=exportParts.join(' / ');
    document.getElementById('exportStatusRow').style.display=d.einspeisung_configured?'flex':'none';
    document.getElementById('modeText').textContent=MODE_NAMES[d.mode]||d.mode;
    document.getElementById('notausState').textContent=d.notaus?'AKTIV':'inaktiv';
    document.getElementById('uptime').textContent=fmtUptime(d.uptime);
    document.getElementById('buildTime').textContent=d.build_time;
    document.getElementById('gitVersion').textContent=d.git_version;
    document.getElementById('heap').textContent=d.heap;
    document.getElementById('loopTiming').textContent=d.loop_avg_ms===null?'Messung läuft':
      d.loop_avg_ms.toFixed(3)+' / '+d.loop_max_ms.toFixed(3)+' ms (Spitze '+d.loop_peak_ms.toFixed(3)+' ms)';
    document.getElementById('measurementAge').textContent='vor '+d.last_measurement_age+'s aktualisiert';
    document.getElementById('rssiBars').innerHTML=rssiBars(d.rssi);
    document.getElementById('dcState').textContent=(d.demand>0)?'aktiv':'aus';
    document.getElementById('dcRow').classList.toggle('blink', d.demand>0);
    document.getElementById('apwarning').style.display=d.ap_mode?'block':'none';
    document.getElementById('fallbackWarning').style.display=d.fallback?'block':'none';

    // Soyo antwortet nicht auf jedem Gerät auf den Status-Request (siehe README)
    // -- "aus" heißt hier nur "keine Antwort", nicht zwingend ein Fehler, deshalb
    // kein rotes "off", nur grün wenn eine Antwort da ist.
    document.getElementById('dotSoyo').classList.toggle('on', !!d.soyo);
    document.getElementById('dotMqtt').classList.toggle('on', !!d.mqtt_connected);
    document.getElementById('dotMqtt').classList.toggle('off', !d.mqtt_connected);

    const soyoSec=document.getElementById('soyoStatus');
    if(d.soyo){
      soyoSec.style.display='block';
      document.getElementById('soyoBattV').textContent=Number(d.soyo.battery_v).toFixed(1);
      document.getElementById('soyoBattA').textContent=Number(d.soyo.battery_a).toFixed(1);
      document.getElementById('soyoAcV').textContent=Number(d.soyo.ac_v).toFixed(0);
      document.getElementById('soyoFreq').textContent=Number(d.soyo.freq).toFixed(1);
      document.getElementById('soyoTemp').textContent=Number(d.soyo.temp).toFixed(1);
    } else {
      soyoSec.style.display='none';
    }

    // Messwertquelle (Shelly/JSON) gibt's nur in den pollenden Modi -- in
    // Static/HttpInterface/MqttSub blenden wir den Punkt komplett aus, statt
    // ihn fälschlich rot/grün zu zeigen.
    const usesSource = (d.mode==3 || d.mode==4 || d.mode==5);
    const sourceItem = document.getElementById('dotSourceItem');
    sourceItem.style.display = usesSource ? 'inline-flex' : 'none';
    if (usesSource) {
      const sourceDot = document.getElementById('dotSource');
      sourceDot.classList.toggle('on', !d.fallback);
      sourceDot.classList.toggle('off', !!d.fallback);
    }
  }).catch(()=>{
    document.getElementById('dotEsp').classList.remove('on');
  });
}

function fmtPower(value){
  return (value===null||value===undefined)?'– W':Number(value).toFixed(1)+' W';
}
setInterval(refreshStatus,2000);
refreshStatus();

let helpLoaded=false;

function switchTab(name){
  ['main','network','device','help'].forEach(t=>{
    document.getElementById('tab-'+t).classList.toggle('active', t===name);
    document.getElementById('tabbtn-'+t).classList.toggle('active', t===name);
  });
  // Help-Inhalt liegt nicht in dieser Seite, sondern in einer eigenen Datei
  // (help_page.h) und wird erst beim ersten Öffnen nachgeladen -- danach
  // bleibt er im DOM und muss nicht erneut abgerufen werden.
  if (name==='help' && !helpLoaded) {
    fetch('/help_content').then(r=>r.text()).then(html=>{
      document.getElementById('tab-help').innerHTML=html;
      helpLoaded=true;
    }).catch(()=>{
      document.getElementById('tab-help').innerHTML='<p class="hint">Hilfe konnte nicht geladen werden.</p>';
    });
  }
}

function notaus(on){
  fetch(on?'/notaus':'/notaus_off').then(refreshStatus);
}

function restart(){
  if(confirm('ESP wirklich neu starten?')) fetch('/restart');
}

function onModeChange(){
  const mode=parseInt(document.getElementById('mode').value);
  document.getElementById('jsonSection').style.display=(mode==5)?'block':'none';
  document.getElementById('staticSection').style.display=(mode==0)?'block':'none';
  const phaseBoxes=['shelly_l1','shelly_l2','shelly_l3'].map(id=>document.getElementById(id));
  const isShelly=(mode==3||mode==4);
  phaseBoxes.forEach(b=>b.disabled=!isShelly);
}

function onStaticToggle(){
  document.getElementById('staticIpFields').style.display=document.getElementById('wifi_static').checked?'block':'none';
}

function onNightToggle(){
  document.getElementById('nightFields').style.display=document.getElementById('night_mode_enabled').checked?'block':'none';
}

function loadConfigForm(){
  fetch('/config').then(r=>r.json()).then(c=>{
    document.getElementById('wifi_ssid').value=c.wifi_ssid||'';
    document.getElementById('wifi_pass').value=c.wifi_pass||'';
    document.getElementById('wifi_ssid2').value=c.wifi_ssid2||'';
    document.getElementById('wifi_pass2').value=c.wifi_pass2||'';
    document.getElementById('wifi_static').checked=!!c.wifi_static;
    document.getElementById('wifi_ip').value=c.wifi_ip||'';
    document.getElementById('wifi_gw').value=c.wifi_gw||'';
    document.getElementById('wifi_mask').value=c.wifi_mask||'';
    document.getElementById('wifi_11n').checked=!!c.wifi_11n;

    document.getElementById('mqtt_enabled').checked=!!c.mqtt_enabled;
    document.getElementById('mqtt_broker').value=c.mqtt_broker||'';
    document.getElementById('mqtt_port').value=c.mqtt_port||1883;
    document.getElementById('mqtt_user').value=c.mqtt_user||'';
    document.getElementById('mqtt_pass').value=c.mqtt_pass||'';
    document.getElementById('mqtt_sub_topic').value=c.mqtt_sub_topic||'';
    document.getElementById('mqtt_input_format').value=c.mqtt_input_format||0;
    document.getElementById('mqtt_pub_topic').value=c.mqtt_pub_topic||'';

    document.getElementById('mode').value=c.mode||0;
    document.getElementById('static_watt').value=c.static_watt;

    document.getElementById('shelly_ip').value=c.shelly_ip||'';
    document.getElementById('shelly_l1').checked=!!c.shelly_l1;
    document.getElementById('shelly_l2').checked=!!c.shelly_l2;
    document.getElementById('shelly_l3').checked=!!c.shelly_l3;
    document.getElementById('shelly_export_ip').value=c.shelly_export_ip||'';
    document.getElementById('shelly_export_l1').checked=!!c.shelly_export_l1;
    document.getElementById('shelly_export_l2').checked=!!c.shelly_export_l2;
    document.getElementById('shelly_export_l3').checked=!!c.shelly_export_l3;

    document.getElementById('json_url').value=c.json_url||'';
    document.getElementById('json_path').value=c.json_path||'';

    document.getElementById('max_power').value=c.max_power;
    document.getElementById('soyo_count').value=c.soyo_count;
    document.getElementById('calibration_offset_w').value=c.calibration_offset_w;
    document.getElementById('fallback_watt').value=c.fallback_watt;
    document.getElementById('tolerance_import_w').value=c.tolerance_import_w;
    document.getElementById('tolerance_export_w').value=c.tolerance_export_w;
    document.getElementById('correction_gain_percent').value=c.correction_gain_percent;
    document.getElementById('poll_interval_ms').value=c.poll_interval_ms;
    document.getElementById('rs485_send_interval_ms').value=c.rs485_send_interval_ms;

    document.getElementById('night_mode_enabled').checked=!!c.night_mode_enabled;
    document.getElementById('night_start').value=pad(c.night_start_h)+':'+pad(c.night_start_m);
    document.getElementById('night_end').value=pad(c.night_end_h)+':'+pad(c.night_end_m);
    document.getElementById('night_max_power').value=c.night_max_power;

    document.getElementById('ota_pass').value=c.ota_pass||'';

    onModeChange();
    onStaticToggle();
    onNightToggle();
  });
}

// Jede Sektion baut nur ihr EIGENES Teil-Objekt -- das ist der ganze Trick
// hinter der Einzelspeicherung: der Server merged nur die enthaltenen Felder
// in die laufende Konfiguration (siehe configMergeFromJsonString in
// storage.cpp), alle anderen Einstellungen bleiben unangetastet.
function timeParts(id){
  const v=document.getElementById(id).value||'00:00';
  const p=v.split(':');
  return [parseInt(p[0])||0, parseInt(p[1])||0];
}

const SECTION_BUILDERS = {
  wlan: () => ({
    wifi_ssid: document.getElementById('wifi_ssid').value,
    wifi_pass: document.getElementById('wifi_pass').value,
    wifi_ssid2: document.getElementById('wifi_ssid2').value,
    wifi_pass2: document.getElementById('wifi_pass2').value,
    wifi_static: document.getElementById('wifi_static').checked,
    wifi_ip: document.getElementById('wifi_ip').value,
    wifi_gw: document.getElementById('wifi_gw').value,
    wifi_mask: document.getElementById('wifi_mask').value,
    wifi_11n: document.getElementById('wifi_11n').checked
  }),
  mqtt: () => ({
    mqtt_enabled: document.getElementById('mqtt_enabled').checked,
    mqtt_broker: document.getElementById('mqtt_broker').value,
    mqtt_port: parseInt(document.getElementById('mqtt_port').value)||1883,
    mqtt_user: document.getElementById('mqtt_user').value,
    mqtt_pass: document.getElementById('mqtt_pass').value,
    mqtt_sub_topic: document.getElementById('mqtt_sub_topic').value,
    mqtt_input_format: parseInt(document.getElementById('mqtt_input_format').value),
    mqtt_pub_topic: document.getElementById('mqtt_pub_topic').value
  }),
  mode: () => ({
    mode: parseInt(document.getElementById('mode').value),
    static_watt: parseInt(document.getElementById('static_watt').value)||0,
    shelly_ip: document.getElementById('shelly_ip').value,
    shelly_l1: document.getElementById('shelly_l1').checked,
    shelly_l2: document.getElementById('shelly_l2').checked,
    shelly_l3: document.getElementById('shelly_l3').checked,
    json_url: document.getElementById('json_url').value,
    json_path: document.getElementById('json_path').value
  }),
  einspeisezaehler: () => ({
    shelly_export_ip: document.getElementById('shelly_export_ip').value,
    shelly_export_l1: document.getElementById('shelly_export_l1').checked,
    shelly_export_l2: document.getElementById('shelly_export_l2').checked,
    shelly_export_l3: document.getElementById('shelly_export_l3').checked
  }),
  regelung: () => ({
    max_power: parseInt(document.getElementById('max_power').value)||1200,
    soyo_count: parseInt(document.getElementById('soyo_count').value)||1,
    calibration_offset_w: parseInt(document.getElementById('calibration_offset_w').value)||0,
    fallback_watt: parseInt(document.getElementById('fallback_watt').value)||0,
    tolerance_import_w: parseInt(document.getElementById('tolerance_import_w').value)||10,
    tolerance_export_w: parseInt(document.getElementById('tolerance_export_w').value)||10,
    correction_gain_percent: parseInt(document.getElementById('correction_gain_percent').value)||50,
    poll_interval_ms: parseInt(document.getElementById('poll_interval_ms').value)||500,
    rs485_send_interval_ms: parseInt(document.getElementById('rs485_send_interval_ms').value)||1100
  }),
  nacht: () => {
    const ns=timeParts('night_start'), ne=timeParts('night_end');
    return {
      night_mode_enabled: document.getElementById('night_mode_enabled').checked,
      night_start_h: ns[0], night_start_m: ns[1],
      night_end_h: ne[0], night_end_m: ne[1],
      night_max_power: parseInt(document.getElementById('night_max_power').value)||0
    };
  },
  ota: () => ({
    ota_pass: document.getElementById('ota_pass').value
  })
};

function saveSection(name){
  const badge = document.getElementById('saved-'+name);
  if (badge) badge.textContent = 'speichert...';

  fetch('/config', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(SECTION_BUILDERS[name]())})
    .then(r => r.text().then(text => ({status: r.status, text})))
    .then(({status, text}) => {
      if (status !== 200) {
        if (badge) badge.textContent = 'Fehler: '+text;
        return;
      }
      if (badge) {
        badge.textContent = text.indexOf('restarting') !== -1 ? 'Gespeichert, startet neu...' : 'Gespeichert';
        setTimeout(() => { badge.textContent=''; }, 4000);
      }
    })
    .catch(() => { if (badge) badge.textContent = 'Fehler beim Speichern'; });
}

function uploadConfig(){
  const f=document.getElementById('uploadFile').files[0];
  if(!f){ alert('Keine Datei gewählt'); return; }
  const reader=new FileReader();
  reader.onload=function(){
    fetch('/config_upload', {method:'POST', headers:{'Content-Type':'application/json'}, body: reader.result})
      .then(()=>alert('Config wiederhergestellt, Neustart...'));
  };
  reader.readAsText(f);
}

loadConfigForm();
</script>
</body>
</html>
)rawliteral";

// Liefert die komplette Webseite von oben aus. send_P (statt send) liest
// dabei direkt aus dem PROGMEM-Flash-Speicher.
static void handleRoot() {
    server.send_P(200, "text/html", PAGE_HTML);
}

// Inhalt des Help-Tabs, ausgelagert nach help_page.h -- wird nur beim ersten
// Öffnen des Tabs per fetch() nachgeladen (siehe switchTab() im <script>
// weiter unten), nicht Teil von PAGE_HTML.
static void handleHelpContent() {
    server.send_P(200, "text/html", HELP_HTML);
}

// Liefert die aktuellen Live-Werte als JSON. Das ist genau die URL, die die
// JavaScript-Funktion refreshStatus() oben in der Webseite alle 2 Sekunden
// per fetch('/status') abruft, um die Anzeige zu aktualisieren -- ohne dass
// die Seite dafür neu geladen werden muss.
static void handleStatus() {
    JsonDocument doc;
    doc["demand"] = g_demand;
    doc["soyo_count"] = config.soyo_count;
    doc["netz"] = g_netzwert;
    if (g_shellyPhaseValid[0]) doc["verbrauch_l1"] = g_shellyPhasePower[0]; else doc["verbrauch_l1"] = nullptr;
    if (g_shellyPhaseValid[1]) doc["verbrauch_l2"] = g_shellyPhasePower[1]; else doc["verbrauch_l2"] = nullptr;
    if (g_shellyPhaseValid[2]) doc["verbrauch_l3"] = g_shellyPhasePower[2]; else doc["verbrauch_l3"] = nullptr;
    doc["einspeisung_configured"] = strlen(config.shelly_export_ip) > 0;
    doc["einspeisung_l1_enabled"] = config.shelly_export_l1;
    doc["einspeisung_l2_enabled"] = config.shelly_export_l2;
    doc["einspeisung_l3_enabled"] = config.shelly_export_l3;
    if (g_shellyExportPhaseValid[0]) doc["einspeisung_l1"] = g_shellyExportPhasePower[0]; else doc["einspeisung_l1"] = nullptr;
    if (g_shellyExportPhaseValid[1]) doc["einspeisung_l2"] = g_shellyExportPhasePower[1]; else doc["einspeisung_l2"] = nullptr;
    if (g_shellyExportPhaseValid[2]) doc["einspeisung_l3"] = g_shellyExportPhasePower[2]; else doc["einspeisung_l3"] = nullptr;
    if (g_shellyExportValid) doc["einspeisung"] = g_shellyExportTotalW; else doc["einspeisung"] = nullptr;
    doc["einspeisung_stale"] = g_shellyExportStale;
    if (g_shellyExportValid) doc["einspeisung_age"] = (millis() - g_lastExportMeasurementMillis) / 1000;
    else doc["einspeisung_age"] = nullptr;
    doc["rssi"] = WiFi.RSSI();
    doc["mode"] = config.mode;
    doc["notaus"] = g_notaus;
    doc["fallback"] = g_fallbackActive;
    doc["mqtt_connected"] = mqttIsConnected();
    doc["uptime"] = millis() / 1000;
    // Compile-Zeitstempel UND Git-Version, beide automatisch (kein manuelles
    // Pflegen nötig, anders als z.B. DISPLAY_FW_VERSION in display.h, die
    // manuell hochgezählt werden muss und deshalb leicht veraltet). GIT_VERSION
    // kommt aus get_git_version.py (siehe platformio.ini, extra_scripts) --
    // "vX.Y.Z" auf einem sauberen Tag, sonst z.B. "vX.Y.Z-3-gabc1234-dirty".
    // Nützlich, um nach einem OTA-Update zweifelsfrei zu sehen, ob die neue
    // Firmware wirklich angekommen ist, statt es aus Nebeneffekten zu schließen.
    doc["build_time"] = String(__DATE__) + " " + String(__TIME__);
    doc["git_version"] = GIT_VERSION;
    doc["heap"] = ESP.getFreeHeap();
    if (g_loopTiming.ready) {
        doc["loop_avg_ms"] = g_loopTiming.averageUs / 1000.0;
        doc["loop_max_ms"] = g_loopTiming.maximumUs / 1000.0;
    } else {
        doc["loop_avg_ms"] = nullptr;
        doc["loop_max_ms"] = nullptr;
    }
    doc["loop_peak_ms"] = g_loopTiming.peakUs / 1000.0;
    doc["ap_mode"] = wifiMgrIsApMode();
    doc["wifi_state"] = (int)wifiMgrGetState();
    doc["last_measurement_age"] = (millis() - g_lastMeasurementMillis) / 1000;

    if (g_soyoStatus.valid) {
        JsonObject soyo = doc["soyo"].to<JsonObject>();
        soyo["battery_v"] = g_soyoStatus.batteryVoltage;
        soyo["battery_a"] = g_soyoStatus.batteryCurrent;
        soyo["ac_v"] = g_soyoStatus.acVoltage;
        soyo["freq"] = g_soyoStatus.frequency;
        soyo["temp"] = g_soyoStatus.temperature;
        soyo["age"] = (millis() - g_soyoStatus.lastUpdate) / 1000;
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// Endpunkt für den Betriebsmodus "HttpInterface" (mode=1): ein externes
// System (z.B. Home Assistant, Node-RED, ein eigenes Script) ruft periodisch
// GET /L1L2L3Auto?Value=<zahl> auf, um der Firmware den aktuellen Netz-
// Messwert zu "pushen". Der Name der URL ist historisch (aus der
// Ursprungs-Firmware übernommen) und bewusst unverändert, damit bestehende
// Integrationen, die schon gegen diese URL sprechen, weiter funktionieren.
static void handleL1L2L3Auto() {
    if (!server.hasArg("Value")) {
        server.send(400, "text/plain", "Missing Value");
        return;
    }
    float val = server.arg("Value").toFloat();
    applyMeasurement(val);
    server.send(200, "text/plain", "OK");
}

static void handleNotaus() {
    g_notaus = true;
    rs485EmergencyStop();
    LOG("Notaus: aktiviert (Web)");
    server.send(200, "text/plain", "OK");
}

static void handleNotausOff() {
    g_notaus = false;
    LOG("Notaus: deaktiviert (Web)");
    server.send(200, "text/plain", "OK");
}

static void handleRestart() {
    server.send(200, "text/plain", "Restarting...");
    LOG("Neustart via Webinterface angefordert");
    scheduleRestart(300);
}

static void handleConfigGet() {
    String json = configToJsonString(config);
    server.sendHeader("Content-Disposition", "attachment; filename=config.json");
    server.send(200, "application/json", json);
}

// Gemeinsame Logik für "Config speichern" (POST /config, von der Web-Maske --
// kann seit der Aufteilung in einzelne Bereiche auch nur ein Teil der Felder
// enthalten, z.B. nur die WLAN-Felder) und "Config wiederherstellen"
// (POST /config_upload, aus einer hochgeladenen Datei). Beide schicken den
// JSON-Text als POST-Body, server.arg("plain") ist bei ESP8266WebServer der
// Weg, an genau diesen rohen Body heranzukommen.
//
// forceRestart=true (config_upload) startet immer neu, unabhängig davon,
// welche Felder in der Datei stehen -- eine vollständige Wiederherstellung
// soll zuverlässig mit einem sauberen, frischen Zustand starten. Bei einer
// Teil-Speicherung aus der Web-Maske (forceRestart=false) entscheidet
// configMergeFromJsonString anhand der enthaltenen Felder, ob ein Neustart
// nötig ist (WLAN/MQTT/OTA) oder die Änderung sofort live wirkt.
static void applyConfigBody(bool forceRestart) {
    String body = server.arg("plain");
    Config newConfig = config; // aktuelle Werte als Basis, damit nicht enthaltene Felder erhalten bleiben

    bool needsRestart = false;
    if (!configMergeFromJsonString(newConfig, body, needsRestart)) {
        server.send(400, "text/plain", "Invalid JSON");
        return;
    }

    config = newConfig;
    saveConfig(config);

    if (forceRestart || needsRestart) {
        server.send(200, "text/plain", "OK, restarting...");
        LOG("Config: gespeichert, Neustart");
        scheduleRestart(300);
    } else {
        server.send(200, "text/plain", "OK");
        LOG("Config: gespeichert (live uebernommen, kein Neustart noetig)");
    }
}

static void handleConfigPost() {
    applyConfigBody(false);
}

static void handleConfigUpload() {
    applyConfigBody(true);
}

static void handleLog() {
    server.send(200, "application/json", getRingbufferJson());
}

// server.on(pfad, methode, handlerFunktion) trägt eine "Route" in den
// Webserver ein: kommt eine Anfrage mit passendem Pfad UND passender
// HTTP-Methode (GET/POST) rein, wird automatisch die angegebene Funktion
// aufgerufen. "/config" kommt bewusst zweimal vor (GET und POST) -- das sind
// zwei unterschiedliche Handler für denselben Pfad, je nach Methode.
void httpServerBegin() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/help_content", HTTP_GET, handleHelpContent);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/L1L2L3Auto", HTTP_GET, handleL1L2L3Auto);
    server.on("/notaus", HTTP_GET, handleNotaus);
    server.on("/notaus_off", HTTP_GET, handleNotausOff);
    server.on("/restart", HTTP_GET, handleRestart);
    server.on("/config", HTTP_GET, handleConfigGet);
    server.on("/config", HTTP_POST, handleConfigPost);
    server.on("/config_upload", HTTP_POST, handleConfigUpload);
    server.on("/log", HTTP_GET, handleLog);
    server.begin();
    LOG("HTTP-Server gestartet");
}

// Wird bei jedem Schleifendurchlauf aus der Haupt-loop() (main.cpp) aufgerufen.
// server.handleClient() schaut nach, ob gerade eine Anfrage reingekommen ist,
// und ruft bei Bedarf den passenden, oben registrierten Handler auf -- non-
// blocking, d.h. ist gerade keine Anfrage da, kehrt die Funktion sofort zurück.
void httpServerLoop() {
    server.handleClient();
    if (restartPending && millis() >= restartAtMillis) {
        ESP.restart();
    }
}
