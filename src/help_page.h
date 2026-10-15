#pragma once

#include <Arduino.h>

// Inhalt des "Help"-Tabs im Webinterface -- ausgelagert in eine eigene Datei,
// damit http_server.cpp (das schon den großen PAGE_HTML-String enthält) nicht
// noch weiter wächst. Wird nicht Teil von PAGE_HTML, sondern über einen
// eigenen Endpunkt (/help_content, siehe http_server.cpp) nachgeladen, sobald
// der Help-Tab das erste Mal geöffnet wird (siehe switchTab() im <script>
// von PAGE_HTML). Reines HTML-Fragment (nur die Karten), kein <html>/<body> --
// wird per innerHTML in den bereits vorhandenen #tab-help-Container
// eingesetzt.
static const char HELP_HTML[] PROGMEM = R"rawliteral(
<div class="card">
  <h2>Ersteinrichtung</h2>
  <p>Beim allerersten Start (oder nach einem Werksreset) öffnet das Gerät einen
  eigenen WLAN-Access-Point <code>SOYO-Setup</code> (Passwort <code>1234567890</code>,
  Adresse <code>10.0.0.1</code>). Damit verbinden, im "Netzwerkkonfiguration"-Tab
  die eigenen WLAN-Zugangsdaten eintragen und "WLAN speichern" klicken — das
  Gerät startet neu und verbindet sich danach mit dem eingetragenen WLAN.
  Anschließend erreichbar über <code>http://soyo.local</code> oder die vom
  Router vergebene IP. Zuletzt im "Gerätekonfig"-Tab unter "Betriebsmodus" den
  gewünschten Modus einstellen und speichern.</p>
  <p>Optional zusätzlich eine Fallback-SSID/Passwort im
  "Netzwerkkonfiguration"-Tab eintragen — wird erst probiert, wenn das
  primäre WLAN eine Weile nicht erreichbar ist (z.B. Handy-Hotspot als
  Notlösung). Primäres WLAN bleibt bevorzugt; ein automatisches Zurückwechseln
  passiert passiv beim nächsten Verbindungsverlust, nicht aktiv während einer
  stabilen Fallback-Verbindung.</p>
</div>

<div class="card">
  <h2>Werksreset</h2>
  <p>GPIO0 (Flash-Taste am Board) beim Einschalten/Reset 5 Sekunden gedrückt
  halten (muss schon <b>vor</b> dem Einschalten gedrückt sein, nicht erst
  danach) → die Konfiguration wird gelöscht, das Gerät startet neu und öffnet
  wieder den Setup-Access-Point.</p>
</div>

<div class="card">
  <h2>Steuerung</h2>
  <p><b>Notaus</b>: setzt den Sollwert sofort auf 0W und sendet das umgehend
  an den Wechselrichter (nicht erst nach dem nächsten Regelzyklus). Solange
  Notaus aktiv ist, wird kein Messwert mehr verarbeitet — die Regelung pausiert
  komplett, unabhängig vom eingestellten Betriebsmodus.</p>
  <p><b>Notaus aufheben</b>: schaltet zurück in den Normalbetrieb. Der Sollwert
  wird ab dem nächsten Regelzyklus wieder ganz normal aus dem aktuellen
  Messwert berechnet, nicht schlagartig auf den alten Wert vor dem Notaus.</p>
  <p><b>Neustart</b>: startet den ESP sofort neu (Soft-Reset), ohne die
  Konfiguration zu verändern. Kurzzeitig nicht erreichbar, bis WLAN wieder
  verbunden ist.</p>
</div>

<div class="card">
  <h2>Betriebsmodi</h2>
  <div class="row"><span>Static</span><span>konstante Gesamtleistung aus "Statische Leistung, Gesamt (W)" (wird durch "Anzahl der Soyos" geteilt), kein externer Messwert nötig</span></div>
  <div class="row"><span>HttpInterface</span><span>externes System pusht Messwert per <code>GET /L1L2L3Auto?Value=x</code></span></div>
  <div class="row"><span>MqttSub</span><span>Messwert kommt per MQTT-Nachricht rein, reagiert sofort ohne Abfrageintervall</span></div>
  <div class="row"><span>Shelly Gen1/Gen2</span><span>Gerät fragt den Shelly-Stromzähler selbst per HTTP ab (Intervall einstellbar), Phasen L1/L2/L3 frei kombinierbar</span></div>
  <div class="row"><span>JSON HTTP Client</span><span>fragt eine beliebige selbst konfigurierbare JSON-URL ab, Wert über einen Punkt-getrennten Pfad ausgelesen</span></div>
  <p class="hint">Bei Shelly/JSON: nach 10 Fehlversuchen in Folge (dazu zählt
  auch ein fehlendes WLAN, nicht nur eine ausbleibende Antwort des
  Messwert-Servers) schaltet die Firmware auf den konfigurierten
  Fallback-Watt-Wert um, bis wieder 3 Anfragen in Folge erfolgreich waren.</p>
</div>

<div class="card">
  <h2>Optionaler Einspeisezähler</h2>
  <p>Die Konfiguration bietet getrennte IP- und Phasenauswahl für einen
  zusätzlichen Shelly Gen2 Pro, der die Einspeisung misst. Er wird mit
  demselben Poll-Intervall wie der Verbrauchszähler abgefragt. Die Werte
  erscheinen auf der Hauptseite und im MQTT-Status; Phasen und Summe sind
  nur verfügbar, wenn die entsprechende Phase konfiguriert und erfolgreich
  gelesen wurde. Dieser Zähler dient ausschließlich dem Monitoring:
  Verbindungsfehler beeinflussen die Regelung und den Fallback nicht.</p>
</div>

<div class="card">
  <h2>Konfiguration: Regelung</h2>
  <div class="row"><span>Max. Leistung pro Inverter (W)</span><span>harte Obergrenze PRO Soyo (z.B. dessen Typenschild-Maximalwert), wird nie überschritten. Bei ungleich starken Invertern am selben Bus den Wert der stärksten Geräte eintragen, ein schwächeres Gerät begrenzt sich selbst</span></div>
  <p class="hint"><b>Wichtig:</b> die Selbstbegrenzung eines schwächeren
  Inverters funktioniert nur, wenn dessen eigener Maximalwert auch tatsächlich
  korrekt in seiner eigenen Firmware/Konfiguration hinterlegt ist (Soyo-eigenes
  Konfigurationstool, unabhängig von diesem ESP8266-Controller hier) --
  vor dem Mischen unterschiedlich starker Geräte am selben Bus unbedingt bei
  jedem einzelnen Inverter prüfen.</p>
  <div class="row"><span>Anzahl der Soyos am Bus (1-12)</span><span>bei Static/Fallback-Watt sowie der laufenden Regelkorrektur wird der GESAMT-Sollwert durch diese Zahl geteilt, damit jeder Soyo seinen Anteil bekommt -- Max. Leistung/Max. Leistung nachts sind davon unabhängig, die gelten pro Inverter</span></div>
  <div class="row"><span>Zähler-Kalibrierung (W)</span><span>wird zu jedem Rohmesswert addiert, bevor Toleranz/Regelung ihn sehen -- gleicht einen systematisch falsch kalibrierten Stromzähler aus, kein Richtungs-Bias</span></div>
  <div class="row"><span>Fallback-Watt, Gesamt</span><span>Sollwert für die Summe aller Soyos, wenn die Messwertquelle wiederholt nicht antwortet (dazu zählt auch fehlendes WLAN)</span></div>
  <div class="row"><span>Toleranz Bezug / Toleranz Einspeisung (W)</span><span>je 5-50W, Standard je 10W -- unabhängige Grenzen pro Richtung, solange der Messwert sie nicht erreicht wird nicht nachgeregelt. Für striktere Nulleinspeisung "Toleranz Einspeisung" enger als "Toleranz Bezug" setzen, sonst beide gleich</span></div>
  <div class="row"><span>Poll-Intervall (ms)</span><span>wie oft Shelly/JSON abgefragt werden (400-2000ms)</span></div>
  <div class="row"><span>RS485-Sendeintervall (ms)</span><span>wie oft ein neuer Sollwert an den Soyo gesendet wird (1000-3000ms, Standard 1100ms); zu häufige oder zu seltene Updates können den Soyo unvorhersehbar reagieren lassen, siehe README</span></div>
  <p class="hint">Änderungen hier wirken sofort, ohne Neustart.</p>
</div>

<div class="card">
  <h2>Konfiguration: Nachtmodus</h2>
  <p>Optional aktivierbar, begrenzt die maximale Leistung PRO Inverter in
  einem Zeitfenster (z.B. 22:00-06:00) zusätzlich zur normalen
  Max.-Leistung-Grenze — z.B. um nachts leiser/schonender zu fahren. Das
  ist keine feste Einspeisung; die normale Regelung läuft im Zeitfenster
  weiter. Das
  Zeitfenster darf über Mitternacht gehen. Wirkt ebenfalls sofort, ohne
  Neustart; die Uhrzeit kommt per NTP aus dem Internet (nur verfügbar,
  sobald das Gerät im WLAN ist).</p>
</div>

<div class="card">
  <h2>Konfiguration: MQTT und OTA</h2>
  <p><b>MQTT</b> verbindet den Controller mit dem eingetragenen Broker, um
  Messwerte zu empfangen (MqttSub) und Statuswerte zu veröffentlichen. Der
  MQTT-Statuspunkt auf der Hauptseite zeigt ausschließlich, ob gerade eine
  Verbindung zum Broker besteht. Änderungen an MQTT-Einstellungen benötigen
  einen Neustart.</p>
  <p>Im Modus <b>MqttSub</b> bestimmt das MQTT-Eingabeformat, ob eine reine
  Watt-Zahl oder Shelly-JSON erwartet wird. Shelly-JSON liest ausschließlich
  <code>total_act_power</code> aus dem Topic
  <code>&lt;Shelly-ID&gt;/status/em:0</code>, ohne Phasenauswahl.
  0 W und negative Werte sind gültig; ungültige Nachrichten aktualisieren
  den Messwert nicht. Die bestehende 60-Sekunden-Messwertüberwachung bleibt
  aktiv und kann bei ausbleibenden Messwerten einen Neustart auslösen.
  Daher regelmäßige Veröffentlichungen sicherstellen und retained Messwerte
  vermeiden: Ihr ursprüngliches Alter kann dieser MQTT-Client nicht erkennen.</p>
  <p><b>OTA</b> ermöglicht den Firmware-Upload über die Update-Seite im WLAN.
  Das eingestellte Passwort schützt diesen Upload; eine Passwortänderung
  wird nach einem Neustart wirksam.</p>
</div>

<div class="card">
  <h2>Speichern: Neustart oder sofort?</h2>
  <p>Jeder Konfigurationsblock hat einen eigenen Save-Button, es gibt keinen
  globalen "Alles speichern" mehr. Nicht mitgeschickte Felder anderer Blöcke
  bleiben dabei unangetastet.</p>
  <div class="row"><span>WLAN, MQTT, OTA-Passwort</span><span>brauchen einen Neustart (Verbindungen werden nur beim Boot aufgebaut)</span></div>
  <div class="row"><span>Betriebsmodus, Regelung, Nachtmodus</span><span>wirken sofort, kein Neustart nötig</span></div>
</div>

<div class="card">
  <h2>Statuspunkte (Hauptseite)</h2>
  <div class="row"><span><span class="status-dot on"></span> ESP</span><span>Webinterface antwortet gerade</span></div>
  <div class="row"><span><span class="status-dot on"></span> Soyo</span><span>RS485-Antwort vom Wechselrichter da (manche Geräte antworten grundsätzlich nie, das ist normal)</span></div>
  <div class="row"><span><span class="status-dot on"></span> Messwertquelle</span><span>nur bei Shelly/JSON-Modus, rot bei Fallback</span></div>
  <div class="row"><span><span class="status-dot on"></span> MQTT</span><span>grün nur, solange eine Verbindung zum MQTT-Broker besteht; rot andernfalls</span></div>
</div>

<div class="card">
  <h2>HTTP-Endpunkte</h2>
  <div class="row"><span><code>GET /status</code></span><span>Live-Status als JSON</span></div>
  <div class="row"><span><code>GET /L1L2L3Auto?Value=x</code></span><span>Messwert setzen (HttpInterface-Modus)</span></div>
  <div class="row"><span><code>GET /notaus</code> / <code>/notaus_off</code></span><span>Notaus setzen/aufheben</span></div>
  <div class="row"><span><code>GET /config</code></span><span>Konfiguration als JSON herunterladen</span></div>
  <div class="row"><span><code>GET /log</code></span><span>letzte 20 Log-Zeilen als JSON</span></div>
  <div class="row"><span><code>GET /update</code></span><span>Firmware-Update-Seite (ElegantOTA)</span></div>
  <p class="hint">Live-Diagnose zusätzlich per Telnet: <code>telnet soyo.local</code> (Port 23).</p>
</div>

<div class="card">
  <h2>Display (optional)</h2>
  <p>Ein angeschlossenes SSD1306-OLED zeigt Live-Betrieb ohne Handy/Laptop:
  im Setup-Modus SSID/IP des Config-Portals, danach kurz Build-Zeitstempel
  und die zugewiesene IP, dauerhaft danach Netz-/Soyo-Leistung, WLAN-Status
  und aktiver Modus. Rein informativ, keine eigenen Einstellungen im
  Webinterface — Details siehe README.</p>
</div>

<div class="card">
  <h2>Vollständige Dokumentation</h2>
  <p>Für Details (RS485-Protokoll, Hardware-Verkabelung, Fallback-Logik,
  Code-Aufbau) siehe die README im Projekt-Repository:</p>
  <a class="btn btn-blue" href="https://github.com/desperado0044/Soyosource-Controller" target="_blank" rel="noopener">Zum GitHub-Repository</a>
</div>
)rawliteral";
