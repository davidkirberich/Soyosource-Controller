# Regelalgorithmus der Original-Firmware (Esp-Soyosource-Controller v1.0.3.3)

Referenzdokument, durch Reverse Engineering (Ghidra, Xtensa-Dekompilat) aus `firmware_v1.0.3.3.bin`
(KlausLi/BavarianSuperGuy, MIT) gewonnen. Der Original-Quellcode ist nicht öffentlich.
Funktionsnamen (`FUN_xxxxxxxx`) sind Ghidra-Adressen. Als Vergleichsbasis für diese Firmware (Neuentwicklung).

**Verlässlichkeit:** Dies ist eine statische Rekonstruktion, keine vollständig verifizierte Spezifikation.
Die Nachprüfung von Regler, Wartetick, Scheduler, HTTP-/JSON-Pfaden und Statusempfang
stützt sich auf Dekompilate und Xtensa-Assembler. Alle sieben aus dem Binary übernommenen
ELF-Segmente wurden bytegleich zum Originalbinary und zum geladenen Ghidra-Programm geprüft.
Binary-SHA256: `d39a87b60274e52abb0178cbe5e69d30e831a677c5a4faaffe74d48ceda4e654`.
Mit ⚠️ markierte Stellen bleiben abgeleitet; reale Laufzeiten und WR-Reaktionen wurden
durch diese Nachprüfung nicht am Gerät verifiziert.

## 1. Überblick
Zero-Feed-In-Regler. Ein Zähler (Shelly 3EM / 3EM Pro / generisches JSON / MQTT / HTTP-Push) liefert die Netzleistung.
Daraus wird die Ausgangsleistung des Soyosource-Wechselrichters (WR) berechnet und per RS485 gesendet.
Laufzeit-Beispiel am Gerät (Webseite `/data`): `SoyoCount 3, MaxPower 2400, WaitSekunden 2, WattsOut 2400`.

## 2. Scheduler (`FUN_4020eff0`)
Läuft aus der Hauptschleife, aber höchstens alle 333 ms (0x14D). Drei Zustände, rotierend 0 → 1 → 2 → 0:

| Zustand | Aktion |
|---|---|
| 0 | Messwert holen (Modus `AutoShellyHttpClient` oder `AutoJsonHttpClient`), Bedarf berechnen, danach **Regelschritt** (`FUN_40209844`) |
| 1 | Regelschritt (nur wenn Wartesperre inaktiv und kein Sendevorgang offen), danach **Frame erneut senden**, sofern WLAN verbunden ist (`FUN_402112e8` = Status == WL_CONNECTED) und, bei aktivem MQTT, die MQTT-Bedingung erfüllt ist |
| 2 | WR-Status abfragen (`FUN_4020edb8`), nur wenn SoyoCount == 1; sonst werden die DC-Werte auf eine Konstante gesetzt (Webseite zeigt `S>1`) |

Ohne längere Blockierung ergibt sich ein Zyklus von ungefähr 3 × 333 ms ≈ 1 s.
Der Scheduler speichert jedoch den vor der Zustandsbearbeitung erfassten Zeitstempel:
Eine lange HTTP-Abfrage kann den nächsten Zustand unmittelbar fällig machen. Ihre Dauer
ist deshalb nicht einfach zusätzlich zu drei vollständigen 333-ms-Pausen zu addieren.
HTTP-Abfragen und Statusempfang blockieren den normalen Scheduler-/Keep-Alive-Pfad.
In Zustand 1 wird der **zuletzt gebildete Frame** unter den genannten Netzwerkbedingungen
wiederholt (Keep-Alive) – nicht nur bei Änderung. Der Sender selbst erzwingt keinen Mindestabstand.
Die Modi `AutoHttpInterface`, `AutoMqtt`, `AutoMqtt2`, `StaticHttpInterface` liefern den Bedarf von außen (HTTP `/L1L2L3Auto?Value=`, `/L1L2L3Static?Value=`, MQTT-Topic).

## 3. Messwert und Bedarf
- Gen1 (`S3emPro` aus): `GET http://<IP>/emeter/<0|1|2>`, Feld `power`, je gewählter Phase L1/L2/L3.
- Gen2 / 3EM Pro: `GET http://<IP>/rpc/EM.GetStatus?id=0`, Felder `a_act_power`, `b_act_power`, `c_act_power`. Alle drei Phasen werden aus einer Antwort gelesen.
- JSON-Modus: beliebige URL plus punktgetrennter `JsonValuePath` (Standard-Beispiel: `StatusSNS.SML.DJ_TPWRCURR`).
- HTTP-Timeout 5000 ms pro Abfrage, blockierend, **kein Fallback**.
- Vor einer Shelly-Messstufe werden alle drei Phasenwerte auf 0 gesetzt. Ein fehlgeschlagener
  Abruf aktualisiert die betroffenen Werte nicht; danach gelangt trotzdem die Phasensumme
  in den Regler. Das kann 0 W oder eine Teilsumme sein, nicht unbedingt der letzte Messwert.
- Gen1: Eine ausgewählte Phase wird bei Ergebnis 0 einmal erneut abgefragt.
  Gen2: Sobald irgendeiner der drei Phasenwerte 0 ist, wird die gesamte RPC-Abfrage einmal
  wiederholt. Dieser Test berücksichtigt die Phasenauswahl nicht; deaktivierte Phasen
  bleiben 0 und lösen ebenfalls eine Wiederholung aus. Ein erfolgreicher erster Abruf
  bleibt erhalten, wenn nur die Wiederholung fehlschlägt.
- Der Rückgabestatus der JSON-Deserialisierung wird in beiden HTTP-Pfaden ignoriert.
  Fehlende/null-Felder werden bei der Zahlenkonvertierung zu 0; numerische Strings werden
  ebenfalls konvertiert. Es gibt keine strenge Prüfung aller ausgewählten Messfelder.
  Im generischen JSON-HTTP-Pfad liefern HTTP-Fehler 0; fehlerhaftes JSON kann 0 oder
  bereits geparste Teilwerte liefern.
- Summe der Phasen wird, wenn SoyoCount ≠ 0, durch **SoyoCount geteilt** → `demand` (RAM 3fff2358, Float → int).
  Vorzeichen: demand > 0 = Netzbezug (WR soll mehr liefern), demand < 0 = Einspeisung.

## 4. Regelschritt (`FUN_40209844`)
Größen: `out` = aktueller Sollwert in W (3fff2360, **keine gemessene Ausgangsleistung**),
`max` = MaxPower (3fff2368, **pro WR**),
`demand` (3fff2358), `prev` = demand beim letzten ausgeführten Schritt (3fff2354).

### 4.1 Totband
Bei SoyoCount ≠ 0 gilt bei jedem Aufruf: Schwelle Bezug `10/n` W, Schwelle Einspeisung `15/n` W, Bias `2,5/n` W.
Vor der Integerquantisierung entspricht das am Zähler +10 W bzw. −15 W.
Durch das Abschneiden von Summe/n auf ganze Watt kann die wirksame Grenze abweichen
(z. B. bei n = 3: 10 W / 3 wird zu 3 und liegt unter der Schwelle 10/3).
Der Schritt wird nur ausgeführt, wenn
- demand > 0 und demand ≥ 10/n, **oder** demand < 0 und |demand| ≥ 15/n, **oder**
- das Force-Flag (3fff2351, Static-Modus) aktiv ist.
Die MaxPower-/SoyoCount-Änderungsflags (3fff2353 / 3fff2352) umgehen den nachfolgenden
Gleichheitsfilter, nicht generell das Totband.
Ist `demand` gleich `prev`, wird im Auto-Modus ohne diese Änderungsflags kein Schritt ausgeführt.
Verglichen wird der bereits auf ganze Watt abgeschnittene Bedarf nach Division durch n,
nicht die Frische einer Antwort: Auch frisch eingetroffene identische Messungen und
verschiedene Rohwerte mit demselben Integerquotienten werden unterdrückt.
Bei demand == 0 gibt es kein Totband: der Schritt läuft, sobald demand ≠ prev ist.
⚠️ Bei SoyoCount == 0 bleiben die Schwellen auf ihrem Initialwert (nicht ermittelt).

### 4.2 Wartesperre (WaitSekunden, Default **2**, RAM 3fff236c)
Ein Bedarfsanstieg startet eine allgemeine Regelsperre. **Auch das Herunterregeln
bleibt während einer bereits aktiven Sperre blockiert**, einschließlich Lastabwurf
und Wechsel zu negativer Netzleistung.
- Ist `WaitSekunden < 1`: Sperre aus.
- Andernfalls wird beim Aufruf die Sperre (3fff2370) **aktiviert**, wenn `prev < demand` (Bedarf gestiegen, d. h. mehr Bezug oder weniger Einspeisung als beim letzten Schritt), kein Force aktiv, und keine Sperre gerade aktiv bzw. abgelaufen ist (3fff2350 == 0).
- Solange die Sperre aktiv ist, wird **kein** Regelschritt ausgeführt (Ausnahme: MaxPower-Änderung).
- Ein periodischer 1-Sekunden-Tick (`FUN_4020951c`, Dekompilat und Assembler geprüft)
  zählt bei aktiver Sperre einen Zähler (3fff2374) hoch. Die Registrierung über
  `FUN_4020acf0` erfolgt mit 1,0 s, umgerechnet auf 1000 ms, und Wiederholung aktiv.
  Bei `Zähler ≥ WaitSekunden` wird die Sperre gelöscht, der Zähler auf 0 gesetzt
  und 3fff2350 = 1 (verhindert sofortiges Wiedereinschalten); dieses Flag wird erst
  nach dem nächsten geänderten oder erzwungenen, gesendeten Schritt wieder gelöscht.
- Es wird nicht geprüft, ob der erhöhte Bedarf während der Sperre bestehen bleibt.
  Ein fallender Messwert hebt die aktive Sperre nicht auf. Die Sperre startet sogar
  vor der Totbandprüfung.
- ⚠️ Der Timer läuft unabhängig vom Beginn der Sperre. Bei WaitSekunden = 2 sind
  idealisiert ungefähr 1–2 s bis zur Freigabe zu erwarten, anschließend gegebenenfalls
  Scheduler-/HTTP-Verzögerung; keine garantierten zwei Sekunden ab Lastanstieg.
- WaitSekunden ist auf der Weboberfläche **nur Anzeige**; Setzen über MQTT (`Topic_WaitSekunden`). Nach NotAus wird er auf 2 zurückgesetzt.

### 4.3 Anpassung (Modus „Auto")
```
if demand >= 0:                           # Bezug -> mehr Leistung
    if out < max:
        if out + demand > max: out = max
        else:                  out = int(float(demand) + bias + float(out))   # Rundung/Abschneiden zur 0 -> ⚠️
    else:                      out = max                                      # Log "Step 1.5"
else:                                     # Einspeisung -> weniger Leistung
    if out > 0 and -demand <= out:
        out = int(float(out + demand) + bias); if out < 0: out = 0
    else:
        out = 0
```
Inkrementelle Sollwertkorrektur mit Faktor 1 auf den zugelassenen Bedarf, plus Bias
`2,5/n`, ohne zusätzliche Rampe. Der bisherige Sollwert ist Teil des Reglerzustands;
die frühere Bezeichnung als reiner P-Regler war regeltechnisch zu pauschal.
Die Float-Hilfsfunktionen wurden aus dem Kontext
zugeordnet (`SUB_4000e2ac` int→float, `SUB_4000c180` float-Addition, `FUN_40243524` float→int) ⚠️.

**0 W am Netzzähler bedeutet Gleichgewicht, nicht einen 0-W-Sollwert.** Ein Fehlerersatz
von 0 stoppt deshalb den WR nicht automatisch. Ein zugelassener Schritt bei demand == 0
kann durch den Bias den bisherigen Sollwert sogar geringfügig erhöhen; danach greift
bei erneut identischem demand wieder der Gleichheitsfilter.

### 4.4 Static-Modus (Force-Flag ≠ 0)
`out = |demand|`, begrenzt auf `max`. Kein Totband, keine Wartesperre. Wird mit `/L1L2L3Static?Value=` bzw. Webfeld „Static Watt" gesetzt.

### 4.5 Senden
Weicht `out` vom vorherigen Wert ab (oder Force), wird der Frame im Puffer neu aufgebaut und sofort gesendet (außer bei NotAus).
Danach wiederholt Zustand 1 des Schedulers den Frame laufend (siehe Abschnitt 2).

## 5. RS485-Frame (4800 Baud 8N1, Half-Duplex mit DE/RE-Umschaltung)
```
24 56 00 21 HI LO 80 CS      HI = out >> 8,  LO = out & 0xFF
CS = (8 - HI - LO) & 0xFF    (gleich (264 - HI - LO) & 0xFF)
0 W  ->  24 56 00 21 00 00 80 08
```
Sendefunktion `FUN_4020969c`: DE/RE aktivieren, 8 Byte über UART (`FUN_4022f27c`).
Statusabfrage (`FUN_4020edb8`): 8 Byte senden (`24 00 …`), 15 Byte Antwort, Byte 0 muss `0x23` sein, sonst „Status 23 not valid".
Dekodiert: DC-Spannung/-Strom (/10), AC-Spannung, Frequenz (/2). DC-Leistung = U × I.

Der Empfangspfad ignoriert die tatsächlich von `readBytes` (`FUN_40229df4`) gelesene
Länge und prüft nur Byte 0. Byte 14 wird mitgeloggt, aber nicht als Prüfsumme validiert;
auch Headerbytes 1–3 werden nicht geprüft. Eine Teilantwort mit erstem Byte `0x23`
kann dadurch als Status übernommen werden. Der Puffer wird vorab mit 0 gefüllt,
aber gegebenenfalls auch für das Lesen vorheriger UART-Daten verwendet.
Der Algorithmus einer möglichen RX-Prüfsumme ist daraus **nicht rekonstruierbar**.
Die oben belegte TX-Prüfsumme darf nicht ungeprüft auf Statusantworten übertragen werden.

## 6. NotAus, MaxPower, SoyoCount
- **NotAus** (`/NotAus`, `FUN_402097ac`): Flag 3fff2378 = 1, Reset der Regelzustände (`FUN_402096e0`: out = 0, demand = 0, Flags gelöscht, WaitSekunden = 2, Frame = 0 W), sofortiger Versand des 0-W-Frames. Solange NotAus aktiv ist, läuft kein Regelschritt und kein Keep-Alive.
- **NotAus aus** (`/NotAusAus`, `FUN_40209758`): gleicher Reset, Flag = 0.
- **MaxPower** (`FUN_40209db4`): Webseite ±50 W, Untergrenze 50 W, Obergrenze ≈ 8950 W (0x22F7 + 0x32). Löst einen erzwungenen Regelschritt aus. Wert gilt **pro WR**.
- **SoyoCount** (`FUN_40209ce4`): 1 bis 5 WR (Webseite `<1SOYO` / `>1SOYO`); der Teiler wirkt auf den Bedarf. Alle WR am Bus erhalten denselben Frame.
- Konfigurationsspeicherung: `/System?ControllerMode=…&ShellyIP=…&L1=…&L2=…&L3=…&Topic=…&HttpGetUrl=…&JsonValuePath=…&ControllerName=…&S3emPro=…` (`FUN_4020caa8`).

## 7. Schwächen der Original-Firmware
1. Kein Fallback bei Ausfall der Messquelle; Fehler können als 0 oder Teilsumme in
   den Regler gelangen. Der Sollwert bleibt oft stehen, wird aber nicht zuverlässig genullt.
2. Blockierende HTTP-Abfragen (Timeout 5 s pro Abruf) in der Hauptschleife;
   mehrere Phasenabrufe/Wiederholungen können eine Messstufe länger blockieren.
3. Totband fest einkompiliert, P-Regler ohne Glättung (Gain 1).
4. Kein Soll/Ist-Abgleich mit der WR-Rückmeldung; Status nur bei SoyoCount == 1.
5. Fehlende Gen2-Felder werden zu 0; JSON-Parserfehler werden nicht geprüft.
6. Aktive Wartesperre verzögert auch negative Korrekturen; identische frische
   Messwerte lösen keine weiteren Korrekturen aus.
7. Statusantwort ohne Prüfung der vollständigen Länge oder einer RX-Prüfsumme.

## 8. Vergleich mit dieser Firmware (davidkirberich/Soyosource-Controller, Stand `master`)

**Historischer Vergleich:** Dieser Abschnitt beschreibt den damaligen Fork-Stand,
nicht die aktuelle Firmware. Seitdem wurden unter anderem Leistungsbegrenzung,
Notaus, Messwertvalidierung, Fallback und Einspeisezähler-Telemetrie geändert.
Die Tabelle und der Hinweis darunter bleiben zur Nachvollziehbarkeit erhalten;
für das aktuelle Verhalten gelten Quellcode und README. Die Unsicherheiten
der Originalanalyse werden dadurch nicht aufgelöst.

| Aspekt | Original 1.0.3.3 | Dieser Fork |
|---|---|---|
| Regelformel | `out += demand + 2,5/n` (Gain 1) | `target = demand_alt + netz/n × Gain` |
| Verstärkung | 100 % | 50 % Default, 10–100 % |
| Totband | +10 / −15 W (Summe), fest | ±10 W Default, 5–50 W je Richtung |
| Bias | +2,5/n | keiner |
| Wartesperre | Anstieg startet Sperre auch gegen Herunterregeln; siehe Abschnitt 4.2 | nicht vorhanden |
| Clamp | 0…MaxPower pro WR | gleich, plus Nachtmodus |
| Sollwertframe / TX-Prüfsumme | siehe Abschnitt 5 | **identisch** |
| Senden | bei Änderung sofort, Keep-Alive ≈ 1 s | zyklisch 1100 ms (1000–3000), immer |
| Messung | blockierend, Timeout 5 s | asynchron, 500 ms (400–2000), Timeout 1 s |
| Messfehler | kein Fallback | 3 Fehlversuche → Fallback-Watt, 3 Erfolge → zurück; Neustart nach 60 s ohne Messwert |
| Status-Abfrage | jeden Zyklus (nur SoyoCount == 1) | alle 10 s, mit Plausibilitätsprüfung |
| Shelly Gen1 | `/emeter/N` | `/status` (`emeters[]`, `total_power`) |
| Shelly Gen2 / 3EM Pro | `/rpc/EM.GetStatus?id=0`, `a/b/c_act_power` | gleiche URL und Felder |

Hinweis zum Fork (zu klären, wenn die Firmware lauffähig ist): Fehlt in der Gen2-Antwort ein Feld, liefert der Fork 0 W als gültigen Messwert.
Das liegt im Totband, der Sollwert bleibt also stehen, ohne dass Fallback oder Watchdog greifen.

## 9. Offene Punkte (nicht verifiziert)
- Startwerte der Totband-Schwellen bei SoyoCount == 0 (RAM 3ffe8644/3ffe8648).
- Laufzeitverhalten der Freigabe bei blockierender Hauptschleife; der periodische
  1000-ms-Timer ist statisch belegt, seine reale Ausführung wurde nicht gemessen.
- Bedeutung/Algorithmus des letzten Bytes der RS485-Statusantwort.
- Messung am echten Gerät (Telnet-/Serial-Log oder RS485-Mitschnitt) würde Wartesperre und Keep-Alive sicher bestätigen.
