<!-- Translated from docs/INSTALL.md. Authoritative version: ../../INSTALL.md -->

# Installation

Diese Anleitung richtet sich an Besitzer einer Kamera vom Typ JOOAN W3-U, die
Anweisungen befolgen und Befehle kopieren können, aber keine Linux-Experten
sind. Maintainer, die eigene Releases erstellen, finden die passende Anleitung
unter [Erstellen und Veröffentlichen](../../../docs/BUILDING.md).

<a id="read-this-first"></a>
## Bitte zuerst lesen

Stand 2026-11-09 stehen unter
[GitHub Releases](https://github.com/ADCDS/jooan-w3u-local-firmware/releases)
folgende Releases bereit:

- **v0.2.21** (2026-10-16) ist das Release, durch das die nummerierten Schritte
  dieser Anleitung führen. Es wird über Wi-Fi installiert, ohne zusätzliche
  Hardware. Sie erhalten eine lokale Webkonsole, SSH, RTSP-Video von beiden
  Objektiven sowie Schwenken/Neigen und Presets per ONVIF für einen NVR wie
  Frigate, und die Kamera kann die JOOAN-Cloud nicht mehr erreichen. Das Video
  stammt weiterhin vom Kameraprogramm des Herstellers, das abgeschottet
  weiterläuft.
- **v0.2.40** (2026-11-09) ist das neueste Release. Es ergänzt den offenen
  Medien-Daemon in [`media/`](../../../media/) und ist der Stand, den der Rest
  dieses Repositorys beschreibt. Inzwischen wurde eine Kamera mit dieser
  Anleitung von der Werksfirmware auf v0.2.40 samt der zugehörigen
  Medienschicht gebracht, und zwar zweimal. Bevor Sie sich dafür entscheiden,
  lesen Sie den Abschnitt [Die Medienschicht](#the-media-layer), einschließlich
  dessen, was getestet wurde und was nicht.
- **Jedes dieser Releases lässt sich nur auf einer Kamera installieren,
  deren Herstellerfirmware mit der des verifizierten Geräts übereinstimmt.**
  Bevor das Installationsprogramm irgendetwas schreibt, vergleicht es die
  Dateien des Herstellers mit denen des verifizierten Geräts: bei v0.2.21 die
  gesamte Anwendungspartition, bei v0.2.40 die zwölf Dateien, von denen dieses
  Release abhängt. Weichen sie ab, verweigert es die Installation und lässt die
  Kamera unverändert. Die Medienschicht stellt anschließend höhere Anforderungen
  an Ihre Anwendungspartition: Jede Datei, die sie beibehält, muss vorhanden
  sein, und die Kamera muss mit den Sensoren vom Typ CV2005 ausgestattet sein.
  Noch weiß niemand, ob heute verkaufte Kameras mit derselben
  Herstellerfirmware ausgeliefert werden.
- Noch gilt kein Release als „unterstützt“. Jedes bisherige Release ist eine
  technische Vorabversion, die an einer einzigen Kamera getestet wurde.

## Drahtlos oder mit Hardware?

| Was | Wie | Zusätzliche Hardware |
|---|---|---|
| Erstinstallation | Über Wi-Fi, mit der integrierten Web-Update-Funktion der Herstellerfirmware | Keine |
| Spätere Updates und Deinstallation | Über Wi-Fi, im Bereich „Firmware“ der Webkonsole | Keine |
| Sicherung des Flash-Speichers der Kamera | Über SSH, nach der Installation ([Schritt 7](#7-back-up-the-camera-over-ssh)) | Keine |
| Sicherung oder Wiederherstellung auf Chip-Ebene | Mit einem SPI-Flash-Programmiergerät und einer SOIC-8-Klemme, bei geöffneter Kamera | Für die Installation optional. Die einzige Möglichkeit, eine Kamera wiederherzustellen, die nicht mehr startet |
| Medienschicht (v0.2.40) | Über Wi-Fi und SSH. Dabei wird die Anwendungspartition der Kamera neu geschrieben | Für die Installation keine. Fällt während des Schreibens der Strom aus, ist die Kamera nur noch mit einem Programmiergerät und einer Sicherung zu retten |

**Für die Installation von v0.2.21 benötigen Sie weder eine microSD-Karte noch
ein USB-Kabel, einen seriellen Adapter oder ein Flash-Programmiergerät, und die
Kamera muss nicht geöffnet werden.** Das Ethernet-Anschlusskabel der Kamera
wird in dieser Anleitung nicht verwendet.

Eine microSD-Karte ist nie erforderlich. Ist eine eingesetzt, schreibt die
Firmware Absturzprotokolle darauf. Die Medienschicht kann in ihrem
Fallback-Startmodus ihre Programmdateien auch von einer Karte ausführen; das
ist jedoch Sache der Maintainer und kein Installationsweg (siehe
[Die Medienschicht](#the-media-layer)).

## Was Sie benötigen

- **Die Kamera.** Es muss eine JOOAN W3-U sein, für die das Typenschild oder
  die Hersteller-App das Modell `JA-A12` angibt. Andere Kameras, die als „W3-U“
  verkauft werden, verwenden andere Chips: Lesen Sie
  [Kompatibilität](../../../docs/COMPATIBILITY.md). Das Installationsprogramm
  prüft das Modell und die Dateien des Herstellers und lehnt eine abweichende
  Kamera ab. Die Platine kann es nicht sehen; die einzige vollständige Prüfung
  ist daher das Öffnen der Kamera (worauf dabei zu achten ist, erklärt
  „Kompatibilität“).
- **Die JOOAN-App für das Smartphone.** Über die App gelangt die Kamera mit der
  Herstellerfirmware in Ihr Wi-Fi-Netzwerk. Dieses Projekt bietet keinen
  anderen Weg, eine Kamera mit Werksfirmware ins Netzwerk zu bringen.
- **Ein Wi-Fi-Router**, bei dem:
  - die Kamera einem WPA2-Netzwerk beitreten kann. Einem Access Point mit
    OpenWrt und einem Funkchip vom Typ Qualcomm ath11k konnte sie nicht
    beitreten; Access Points auf Realtek-Basis funktionierten (siehe
    [Kompatibilität](../../../docs/COMPATIBILITY.md#wi-fi-access-point-interoperability));
  - Sie den Internetzugang eines einzelnen Geräts sperren können. Je nach
    Router heißt das Kindersicherung, Zugriffskontrolle oder Firewall-Regel;
  - sich Ihr PC im selben Netzwerk wie die Kamera befindet.
- **Ein PC** unter Windows 10 oder neuer, macOS oder Linux, mit:
  - Python 3.9 oder neuer;
  - dem Python-Paket `cryptography`;
  - einem SSH-Client. Er ist in Windows 10 und neuer, macOS und Linux bereits
    enthalten.
- **Etwa 30 Minuten** Zeit und eine stabile Stromversorgung für die Kamera
  (5 V, 1,5 A). Trennen Sie die Kamera während der Installation keinesfalls vom
  Strom.

## Schritt für Schritt

### 1. Die Kamera mit der JOOAN-App einrichten

Folgen Sie der normalen Einrichtung in der App und verbinden Sie die Kamera mit
dem Wi-Fi-Netzwerk, in dem sie dauerhaft bleiben soll. Das Netzwerk der Kamera
später zu wechseln, ist aufwendiger (siehe [Schritt 8](#8-use-the-camera)). Die
App koppelt die Kamera über die JOOAN-Cloud; erledigen Sie dies also, solange
die Kamera noch das Internet erreichen kann, denn Schritt 3 sperrt den Zugang
anschließend. (Die Herstellerfirmware bietet außerdem einen Hotspot für die
Einrichtung und einen DHCP-Weg über Ethernet, mit denen sich die App womöglich
umgehen ließe; keiner der beiden wurde mit dieser Anleitung getestet.)

Bietet die App ein Firmware-Update für die Kamera an, lehnen Sie es ab. Eine
neuere Herstellerfirmware verändert die Dateien, die das Installationsprogramm
prüft, und das Installationsprogramm lehnt die Kamera dann ab.

**Es hat funktioniert, wenn** die App ein Live-Video anzeigt.

### 2. Die IP-Adresse der Kamera ermitteln

Sehen Sie in der Liste der verbundenen Geräte Ihres Routers nach. Die
Herstellerfirmware fragt beim Router unter dem Namen `JAIPC` nach einer
Adresse. Die MAC-Adresse der Kamera finden Sie in den Geräteinformationen der
App. Es empfiehlt sich, die Adresse in den DHCP-Einstellungen des Routers für
die Kamera zu reservieren, damit sie sich nicht ändert.

**Es hat funktioniert, wenn** die Kamera von Ihrem PC aus auf `ping CAMERA_IP`
antwortet. Ersetzen Sie hier und im Folgenden `CAMERA_IP` durch die ermittelte
Adresse, zum Beispiel `192.168.1.50`.

### 3. Den Internetzugang der Kamera sperren

Sperren Sie im Router den Internetzugang der Kamera, und zwar ausschließlich
den der Kamera. Lassen Sie Ihren PC im selben Netzwerk.

Bei jedem Einschalten laufen für einige Sekunden der Webserver und die
Update-Funktion des Herstellers, bevor diese Firmware übernimmt. Die Sperre
sorgt dafür, dass dieses Zeitfenster ohne Internetzugang bleibt. Ab jetzt kann
die JOOAN-App diese Kamera nicht mehr erreichen. Das ist normal.

**Es hat funktioniert, wenn** die App die Kamera als offline anzeigt und
`ping CAMERA_IP` weiterhin eine Antwort liefert.

### 4. Den PC vorbereiten

Laden Sie Folgendes in einen neuen Ordner herunter:

1. Den **Quellcode von v0.2.21** unter
   <https://github.com/ADCDS/jooan-w3u-local-firmware/archive/refs/tags/v0.2.21.zip>.
   Entpacken Sie ihn. Dabei entsteht ein Ordner `jooan-w3u-local-firmware-0.2.21`.
2. Diese sechs Dateien von der
   [Release-Seite von v0.2.21](https://github.com/ADCDS/jooan-w3u-local-firmware/releases/tag/v0.2.21).
   Legen Sie sie gemeinsam in einen Ordner namens `release` neben dem
   Quellcode-Ordner: `JOOAN_FW_PKG`, `JOOAN_FW_PKG.sha256`, `JOOAN_UNINSTALL`,
   `JOOAN_UNINSTALL.sha256`, `manifest.json` und `manifest.json.sig`.

Der Quellcode muss von v0.2.21 stammen, nicht vom aktuellen Branch `main`. Das
aktuelle Upload-Werkzeug weist das Paket von v0.2.21 mit der Meldung `signed release target/ABI contract does
not match this repository` zurück.

Installieren Sie `cryptography`.

Linux oder macOS:

```sh
python3 -m pip install --user cryptography
```

Unter Debian oder Ubuntu funktioniert auch
`sudo apt install python3-cryptography`.

Windows (Eingabeaufforderung):

```bat
py -m pip install cryptography
```

**Es hat funktioniert, wenn** dieser Befehl eine Versionsnummer ausgibt:

```sh
python3 -c "import cryptography; print(cryptography.__version__)"
```

Geben Sie unter Windows hier und in Schritt 5 `py` statt `python3` ein.

### Drahtlose Auslieferung: Wie die Kamera das Paket tatsächlich erhält

Lesen Sie dies vor Schritt 5. Die werkseitige Update-Funktion arbeitet per
**Pull** aus der Cloud, nicht per Upload. `tools/upload_ota.py` stößt sie an,
doch was das Werkzeug per POST sendet, wird ignoriert: Die Kamera lädt die
Firmware anschließend über TLS von einer fest vorgegebenen Cloud-URL herunter,
`https://use1api.jooaniot.com/fw/JOOAN_FW_PKG`. Eine Installation hat daher
über das Ausführen des Werkzeugs hinaus zwei Voraussetzungen:

1. **`use1api.jooaniot.com` muss für die Kamera auf den PC aufgelöst werden,
   der das Paket bereitstellt.** Genau das übersehen die meisten. Die Kamera
   **ignoriert den DNS-Server, den Ihr Router per DHCP verteilt**, und fragt
   fest einprogrammierte öffentliche Resolver ab (223.6.6.6 und 8.8.8.8).
   „Ihren DNS“ zu ändern, bewirkt nichts. Die Umleitung muss im Netzwerkpfad
   erfolgen. Zwei Wege funktionieren:

   - **Ein Router, der diesen Namen für die Kamera überschreibt.** Unter
     OpenWrt sind das eine einzige dnsmasq-Zeile,
     `address=/use1api.jooaniot.com/<PC-IP>`, sowie eine Firewall-Regel, die
     **den Port-53-Verkehr der Kamera selbst auf den Resolver des Routers
     umleitet** (ein DNAT von UDP/TCP 53 der Kamera auf den Router), denn
     andernfalls umgeht die Kamera den DNS des Routers. Beschränken Sie beides
     auf die MAC-Adresse der Kamera.
   - **Ein eigenes Gerät zur DNS-Umleitung im Subnetz der Kamera**, das diesen
     einen Namen mit der Adresse des PCs beantwortet und alles andere
     weiterleitet; auch hier wird Port 53 der Kamera auf dieses Gerät
     umgeleitet.

   Den Rest der Cloud müssen Sie nicht nachbilden. Umgeleitet wird nur dieser
   eine Hostname, und nur für den Firmware-Download.

2. **Der PC muss das Paket auf TCP 443 bereitstellen.** `tools/serve_ota.py`
   tut genau das und nichts anderes: Es beantwortet `/fw/JOOAN_FW_PKG` mit der
   angegebenen Datei über ein selbstsigniertes Zertifikat (die Kamera prüft es
   nicht) und beantwortet jeden anderen Pfad mit 404. Es gibt sich nicht als
   Cloud-API aus, leitet nichts an vorgelagerte Server weiter und rührt die
   Zugangsdaten des Geräts nicht an.

Läuft auf Ihrer Kamera bereits jooan-local (ein späteres Update, keine
Erstinstallation), gilt nichts davon: Updates laufen dann über die
authentifizierte API `/api/v1/update`, nicht über den Pull aus der Cloud.

### 5. Installieren

Stellen Sie das Paket bereit. Führen Sie dazu im Ordner
`jooan-w3u-local-firmware-0.2.21` in einem ersten Terminal Folgendes aus (zum
Binden von Port 443 sind erweiterte Rechte nötig; unter Linux `sudo`
verwenden):

```sh
sudo python3 tools/serve_ota.py --pkg ../release/JOOAN_FW_PKG
```

Das Skript gibt `[ready] https://0.0.0.0:443/fw/JOOAN_FW_PKG` aus und erzeugt
ein Wegwerfzertifikat für `use1api.jooaniot.com`. Lassen Sie es laufen.
Richten Sie die DNS-Umleitung aus dem vorigen Abschnitt ein, damit die Kamera
diesen Server erreicht.

Lösen Sie dann in einem zweiten Terminal im selben Ordner das Update auf der
Kamera aus:

```sh
python3 tools/upload_ota.py --host CAMERA_IP --pkg ../release/JOOAN_FW_PKG
```

Das Werkzeug prüft die Signatur des Pakets, stößt die Update-Funktion des
Herstellers auf der Kamera an (die das Paket von dem oben gestarteten Server
herunterlädt) und wartet, bis die Kamera mit der neuen Firmware neu startet.
Sobald die Kamera das Paket abruft, gibt das Server-Terminal
`[served] /fw/JOOAN_FW_PKG -> ... bytes` aus. Rechnen Sie mit ein bis fünf
Minuten. Trennen Sie die Kamera währenddessen nicht vom Strom.

**Es hat funktioniert, wenn** die letzte Zeile lautet:

```text
[+] jooan-local is active
```

Falls es fehlschlägt:

| Meldung | Bedeutung und Vorgehen |
|---|---|
| `error: signed release target/ABI contract does not match this repository` | Sie verwenden eine andere Version des Quellcodes. Nehmen Sie das ZIP-Archiv von v0.2.21 aus Schritt 4 |
| `... requires the Python 'cryptography' package` | Installieren Sie es wie in Schritt 4 beschrieben |
| `[-] unexpected OEM response HTTP ...` | Die Update-Funktion des Herstellers auf der Kamera hat die Anfrage zum Auslösen abgelehnt. Brechen Sie hier ab |
| Das Server-Terminal gibt nie `[served] ...` aus | Die Kamera konnte Ihren Server nicht erreichen. Ursache ist fast immer die DNS-Umleitung: Prüfen Sie, ob `use1api.jooaniot.com` **aus Sicht der Kamera** auf Ihren PC aufgelöst wird und ob Port 53 der Kamera umgeleitet ist (den per DHCP verteilten DNS ignoriert sie). Stellen Sie außerdem sicher, dass Port 443 auf dem PC offen ist |
| `[-] package upload completed but jooan-local did not become healthy` | Die Kamera hat das Paket angenommen, aber diese Firmware ist nicht gestartet. Höchstwahrscheinlich hat das Installationsprogramm abweichende Herstellerdateien vorgefunden und die Kamera abgelehnt, bevor es etwas geschrieben hat. Die Kamera behält ihre Herstellerfirmware. Brechen Sie hier ab und lesen Sie [Wenn etwas schiefgeht](#if-something-goes-wrong) |
| Überhaupt keine Antwort | Prüfen Sie `CAMERA_IP` und ob sich PC und Kamera im selben Netzwerk befinden |

### 6. Anmelden

Öffnen Sie `http://CAMERA_IP/` in einem Browser und melden Sie sich an:

```text
username: admin
password: admin
```

Das Passwort bleibt `admin`, bis Sie es im Bereich „System“ ändern. Dasselbe
Passwort gilt für SSH und RTSP; wenn Sie es ändern, ändert es sich für alle
drei.

**Es hat funktioniert, wenn** `http://CAMERA_IP/api/v1/setup/status` den Wert
`"release_version":"0.2.21"` anzeigt.

<a id="7-back-up-the-camera-over-ssh"></a>
### 7. Die Kamera über SSH sichern

Empfohlen. Dabei wird ohne zusätzliche Hardware eine Kopie jeder
Flash-Partition auf Ihrem PC gespeichert. Nur mit dieser Kopie lässt sich diese
Kamera wiederherstellen, falls ein späteres Update schiefgeht: Die darin
enthaltenen Herstellerdateien sind nirgends veröffentlicht. Um eine Kamera
wiederherzustellen, die nicht mehr startet, ist zusätzlich ein
Flash-Programmiergerät nötig (siehe
[Wiederherstellung](./RECOVERY.md#external-spi-recovery)).

Die Kamera hat sieben Partitionen, `mtd0` bis `mtd6`. Dieser Befehl listet sie
auf:

```sh
ssh admin@CAMERA_IP cat /proc/mtd
```

Sichern Sie anschließend jede Partition. Führen Sie jeden Befehl aus und geben
Sie das Passwort ein, wenn Sie dazu aufgefordert werden. Verwenden Sie unter
Windows die Eingabeaufforderung, nicht PowerShell. Die Umleitung mit `>` in
Windows PowerShell beschädigt Binärdateien.

```sh
ssh admin@CAMERA_IP "cat /dev/mtdblock0" > camera-mtd0.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock1" > camera-mtd1.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock2" > camera-mtd2.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock3" > camera-mtd3.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock4" > camera-mtd4.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock5" > camera-mtd5.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock6" > camera-mtd6.bin
```

**Es hat funktioniert, wenn** die sieben Dateigrößen zusammen 8388608 Bytes
(8 MiB) ergeben und bei `camera-mtd4.bin` (der Anwendungspartition) die
MD5-Summe auf der Kamera:

```sh
ssh admin@CAMERA_IP md5sum /dev/mtdblock4
```

mit der auf dem PC übereinstimmt: `md5sum camera-mtd4.bin` (Linux),
`md5 camera-mtd4.bin` (macOS) oder `certutil -hashfile camera-mtd4.bin MD5`
(Windows).

Diese Dateien enthalten die Identität dieser Kamera, ihr Wi-Fi-Passwort und
ihre Schlüssel. Behandeln Sie sie vertraulich, geben Sie sie niemals weiter und
schreiben Sie sie niemals auf eine andere Kamera.

<a id="8-use-the-camera"></a>
### 8. Die Kamera verwenden

Mit v0.2.21:

- **Video:** RTSP auf Port 554, Benutzer `admin` und das
  Administratorpasswort. `rtsp://CAMERA_IP:554/live/ch0` ist das Objektiv im
  Schwenk-Neige-Kopf, `rtsp://CAMERA_IP:554/live/ch1` das feste Objektiv.
- **Schwenken/Neigen und Presets:** ONVIF unter `CAMERA_IP`, Port 80, gleicher
  Benutzer und gleiches Passwort.
- **SSH:** `ssh admin@CAMERA_IP`, gleiches Passwort.
- **Wi-Fi:** Die Kamera bleibt in dem Netzwerk, das die App eingerichtet hat.
  Um sie in ein anderes Netzwerk zu bringen, geben Sie das neue Netzwerk in der
  Webkonsole unter Network → Wi-Fi ein, stellen es bereit und übernehmen es,
  sobald sich die Kamera wieder verbunden hat. Eine Änderung, die nie
  übernommen wird, wird automatisch zurückgenommen.
- **Uhr:** Die Kamera hat keine batteriegepufferte Uhr. Klicken Sie nach einem
  Stromausfall unter System → Camera time auf
  „Sync time & zone to this device“.

Diese Funktionen beschreibt die Dokumentation von v0.2.21. Lesen Sie sie in der
Fassung dieses Tags:
[README](https://github.com/ADCDS/jooan-w3u-local-firmware/blob/v0.2.21/README.md)
und [API](https://github.com/ADCDS/jooan-w3u-local-firmware/blob/v0.2.21/docs/API.md).
Die Dokumente auf `main` beschreiben 0.2.40.

<a id="updating-later"></a>
## Spätere Updates

Ein neueres veröffentlichtes Release installieren Sie über die Webkonsole.
Wählen Sie im Bereich „Firmware“ die zugehörige Datei `JOOAN_FW_PKG` aus,
warten Sie auf „signature verified“ und wenden Sie sie dann an. Die Kamera
startet neu und ist etwa 90 Sekunden lang offline. Ein Release wird nur
angenommen, wenn es von diesem Projekt signiert und neuer als das installierte
ist.

**Aktualisieren Sie eine Kamera mit v0.2.21 nur dann auf 0.2.22 oder neuer,
wenn Sie unmittelbar danach die Medienschicht installieren.** Ab 0.2.22 liefert
die Plattform selbst kein Video mehr; bis zur Installation der Medienschicht
hat die Kamera also kein Video.

Releases nach 0.2.21 antworten nur, wenn sie über eine private IP-Adresse oder
über den eigenen Namen der Kamera angesprochen werden. Der Name lautet
`jooan-w3u`, sofern Sie ihn nicht unter Network → Local name ändern. Er kann
allein stehen oder auf `.local`, `.lan`, `.home.arpa` oder `.internal` enden,
etwa `jooan-w3u.local` oder `jooan-w3u.lan`. Jeder andere Name erhält
`403 host_rejected` (siehe
[Netzwerksicherheit](../../../docs/NETWORK-SECURITY.md#host-and-origin-checks)).

## Deinstallation

Wenden Sie im Bereich „Firmware“ die Datei `JOOAN_UNINSTALL` des installierten
Releases an. Nach dem Neustart läuft die Herstellerfirmware der Kamera wieder
ohne Abschottung: Der Webserver des Herstellers ist zurück, und die Kamera kann
die Cloud wieder erreichen, sofern der Router sie nicht weiterhin sperrt. Die
Kamera behält die Wi-Fi-Einstellungen der Herstellerfirmware. Die Einstellungen
der Plattform, einschließlich eines dort übernommenen Wi-Fi-Netzwerks, werden
gelöscht. Details stehen unter
[Wiederherstellung](./RECOVERY.md#booting-camera-rollback).

**Ist die Medienschicht installiert, wird sie durch die Deinstallation nicht
entfernt.** Ihre Dateien in `/opt/jooanipc-open` bleiben erhalten, darunter das
Stream-Passwort und der private TLS-Schlüssel; löschen Sie sie daher vorher
über SSH: `rm -rf /opt/jooanipc-open`. Auch die Anwendungspartition bleibt in
ihrer reduzierten Form, ohne die Sprachansagen des Herstellers und ohne die
Treiber anderer Sensoren. **Versuchen Sie keinesfalls, sie auf einer laufenden
Kamera zurückzuschreiben:** Das Programm, das aus dieser Partition läuft,
stürzt ab, während sie gelöscht wird, und kommt es vor dem Ende des
Schreibvorgangs zu einem Reset, lässt sich die Kamera nur noch mit einem
Flash-Programmiergerät retten. Um das Original zurückzuspielen, sind ein
Flash-Programmiergerät und die von Ihnen gesicherte Datei `my-appfs.img`
erforderlich.

<a id="if-something-goes-wrong"></a>
## Wenn etwas schiefgeht

- **Die Kamera startet nach Schritt 5 ihre Herstellerfirmware.** Hat das
  Installationsprogramm die Kamera abgelehnt, hat es nichts geschrieben, und
  die Kamera ist unverändert. Sie können zur JOOAN-App zurückkehren, indem Sie
  die Sperre im Router aufheben.
- **Webkonsole oder SSH antworten nach einem Update nicht mehr.** Schalten Sie
  die Kamera einmal aus und wieder ein und warten Sie drei Minuten. Antwortet
  sie dann immer noch nicht, folgen Sie der Anleitung unter
  [Wiederherstellung](./RECOVERY.md).
- **Die Kamera startet überhaupt nicht mehr.** Nur eine Wiederherstellung Ihrer
  Sicherung auf Chip-Ebene mit einem Flash-Programmiergerät bringt sie zurück
  ([Wiederherstellung](./RECOVERY.md#external-spi-recovery)).

Wenn Sie um Hilfe bitten, veröffentlichen Sie niemals Flash-Images, Passwörter
oder Schlüssel (siehe
[Sicherheitsrichtlinie](../../../SECURITY.md#reporting-a-vulnerability)).

<a id="the-media-layer"></a>
## Die Medienschicht

Ab 0.2.22 stammen Video, ONVIF und Schwenken/Neigen vom offenen Medien-Daemon
[jooanipc-open](../../../media/) statt vom Programm des Herstellers. Bei einem
nativen Start, seit 0.2.23 die Voreinstellung, wird das Programm des
Herstellers gar nicht erst gestartet. Diesen Aufbau beschreibt der Rest dieses
Repositorys.

**Diese gesamte Kette ist jetzt zweimal auf einer echten Kamera von der
Werksfirmware bis zum offenen Software-Stack durchgelaufen.** Eine Testkamera
wurde in den echten Werkszustand zurückversetzt, ohne Hersteller-App ins
Wi-Fi-Netzwerk eingebunden, drahtlos (OTA) auf die Plattform aktualisiert und
anschließend mit einem Medien-Image geflasht, das aus ihrer eigenen Partition
erstellt wurde. Sie lieferte Video über RTSP und überstand mit dem offenen
Software-Stack einen Kaltstart. Was der erste Durchlauf geprüft hat und was
noch dünn abgesichert ist:

| Schritt | Geprüft |
|---|---|
| Rückkehr zur Werksfirmware | Die Testkamera wurde wieder mit der Werkspartition geflasht (bei bedientem Watchdog und mit Rollback-Kopie im RAM) und startete die echte, noch nicht eingerichtete Werksfirmware |
| Einrichtung ohne App | Von einem PC in der Nähe aus wurde die Kamera mit `GET /goform/setupWifiInfo` über ihren SoftAP ins Wi-Fi-Netzwerk gebracht; sie meldete Erfolg und trat dem Netzwerk bei |
| Drahtlose Installation der Plattform | Nachdem die Cloud auf einen lokalen Server umgeleitet war, rief die Kamera das signierte Plattformpaket ab, wendete es an und startete es |
| Das Image | Aus der echten Werkspartition erstellt; Byte für Byte dieselben Dateien wie ein nachweislich funktionierendes Medien-Image |
| Die Konfiguration | Was das Installationsprogramm schreibt, besteht die eigene Prüfung des ausgelieferten Daemons |
| Das Installationsprogramm | Seine Entscheidungen, seine Reihenfolge und seine Vorabprüfung, sowohl gegen eine Nachbildung als auch gegen die echte Kamera; es verweigerte korrekterweise die Installation, bis Plattform und Wi-Fi bereit waren |
| Das Flash-Skript `first-appfs.sh` | Wurde tatsächlich ausgeführt: Es flashte das Medien-Image und behielt die Rollback-Kopie im RAM, und der offene Daemon qualifizierte den nativen Start und bediente den Watchdog |
| Übersteht einen Neustart | Nach vollständigem Aus- und Wiedereinschalten startete die Kamera wieder mit dem offenen Medien-Image und lieferte RTSP, ohne Rollback auf die Werksfirmware |
| Installation von 0.2.39 ab Werksfirmware | In diesem Durchlauf über dieselbe Update-Funktion erfolgt |

Zwei kleinere Mängel, die bei diesem Durchlauf auffielen, wurden in v0.2.40
behoben; anschließend lief die gesamte Kette mit diesem Release ein zweites Mal
ab Werksfirmware durch:

- Ein **offenes** Wi-Fi-Netzwerk (ohne Passwort) lässt sich jetzt über die
  Plattform übernehmen (der Beitritt erfolgt mit `key_mgmt NONE`);
  WPA2-Netzwerke funktionieren wie bisher.
- Bei einer frisch installierten Plattform kann der Platz im 384 KiB großen
  `/opt` für die erste Medieninstallation knapp nicht ausreichen. Das
  Installationsprogramm entfernt jetzt automatisch die regenerierbare
  Werksdatei `/opt/conf/config.org`, wenn der Platz nicht reicht und die
  Werkskopie zur Wiederherstellung vorhanden ist (beim Start der Werksfirmware
  wird diese Datei aus `/mnt/mtd/etc` neu erzeugt).

Schlägt die Rückleseprüfung nach dem Flashen fehl, stellt das Skript die
Partition aus einer Kopie im RAM wieder her. **Fällt während des Schreibens der
Strom aus, lässt sich die Kamera nur noch mit einem Flash-Programmiergerät und
einer Sicherung dieser Partition retten** (siehe
[Wiederherstellung](./RECOVERY.md#appfs-reflash-recovery)). Das Flashen ist
inzwischen auf einer echten Kamera über die beiden Durchläufe hinweg mehrfach
vollständig durchgelaufen, jedoch nur auf diesem einen Gerät; erstellen Sie
daher zuerst eine vollständige Sicherung aller
Partitionen (das Installationsprogramm erledigt das) und sorgen Sie dafür, dass
die Kamera während der wenigen Minuten, die der Vorgang dauert, durchgehend
stabil mit Strom versorgt wird.

Sie benötigen einen Computer mit Linux oder WSL, auf dem `squashfs-tools`,
Python 3, OpenSSH und OpenSSL installiert sind, sowie die Dateien von v0.2.40
von der Release-Seite.

1. **Die Plattform installieren.** Führen Sie die Schritte 4 und 5 mit dem
   Quellcode und den Dateien von v0.2.40 aus, oder aktualisieren Sie ausgehend
   von v0.2.21 wie unter [Spätere Updates](#updating-later) beschrieben. Die
   Kamera hat nun bis zum letzten Schritt kein Video. Die Webkonsole bleibt
   unter `http://CAMERA_IP/` erreichbar.
2. **Die Kamera sichern** ([Schritt 7](#7-back-up-the-camera-over-ssh)).
3. **Das Wi-Fi-Netzwerk der Kamera übernehmen**, und zwar in der Webkonsole
   unter Network → Wi-Fi, auch wenn es das Netzwerk ist, in dem sich die Kamera
   bereits befindet. Ein nativer Start verwendet nur das dort übernommene
   Netzwerk, nicht das von der JOOAN-App eingerichtete.
4. `jooanipc-open-*.tar.gz` und `libimp-*.tar.gz` nebeneinander **entpacken**
   und im Ordner `jooanipc-open-*` ein Terminal öffnen.
5. **Die Anwendungspartition Ihrer Kamera kopieren und daraus Ihr Image
   erstellen.** Beide enthalten Dateien des Herstellers; behandeln Sie sie
   daher vertraulich:

   ```sh
   ssh admin@CAMERA_IP 'cat /dev/mtdblock4' > my-appfs.img
   sh media/tools/build-appfs.sh --first my-appfs.img bin/jooanipc-open ../libimp-*/libimp.so bin/libatomic.so.1 my-bundle
   ```

   Das Skript lehnt eine Partition ab, der eine benötigte Datei fehlt oder die
   ein anderes Herstellerprogramm enthält als das, auf das die Plattform
   festgelegt ist.

6. **Die Kamera prüfen.** Dabei wird nichts verändert:

   ```sh
   CAMERA_PASSWORD=admin python3 media/scripts/install-camera.py my-bundle --camera CAMERA_IP
   ```

7. **Installieren** und dabei das Passwort für die Videostreams der Kamera
   festlegen:

   ```sh
   CAMERA_PASSWORD=admin STREAM_PASSWORD=your-choice python3 media/scripts/install-camera.py my-bundle --camera CAMERA_IP --yes-untested
   ```

   Trennen Sie die Kamera nicht vom Strom. **Es hat funktioniert, wenn** die
   letzte Zeile meldet, dass das Release läuft. Die Admin-Oberfläche ist jetzt
   unter `https://CAMERA_IP/` erreichbar, mit dem eigenen Zertifikat der
   Kamera; der Browser warnt deshalb einmalig. Melden Sie sich mit dem
   Admin-Passwort an. `http://CAMERA_IP/` leitet jetzt nur noch dorthin
   weiter.

Der Start überwacht sich selbst: Verzeichnet ein nativer Start einen Fehler
oder wird er dreimal hintereinander vorzeitig abgebrochen, fällt der nächste
Start auf das Programm des Herstellers zurück. Dasselbe gilt für einen Start,
der nach 5 Minuten noch kein Gateway hat, allerdings nur, bis sich ein nativer
Start einmal qualifiziert hat (siehe
[Wiederherstellung](./RECOVERY.md#a-media-change-that-stops-native-boot)).

**Unter Windows** erstellen Sie das Bundle in WSL und führen das
Installationsprogramm dann mit `py` in der Eingabeaufforderung aus, wobei Sie
jedes Passwort mit `set` setzen. Es verwendet das mit Windows mitgelieferte
OpenSSH und benötigt `openssl` im Suchpfad; das ist in Git for Windows
enthalten. OpenSSH 8.4 oder neuer (Windows 11) übernimmt das Passwort aus
`CAMERA_PASSWORD`; die Version von Windows 10 fragt bei jeder Verbindung
danach. Hinterlegen Sie daher zuerst einen SSH-Schlüssel in der Webkonsole.
Auch unter Windows wurde dies bisher nicht erprobt.

**Spätere Medien-Updates** gehen von der aktuellen Partition Ihrer Kamera aus,
mit `build-appfs.sh` ohne `--first` und einer `bundle.json`, die Sie dafür
selbst schreiben (siehe
[Release-Bundles für die Kamera](../../../media/docs/hardware.md#camera-release-bundles)).
Das Installationsprogramm benötigt dann zusätzlich `MEDIA_PASSWORD`, das
Stream-Passwort, um das Ergebnis zu überprüfen. Das Formular System → Media der
Admin-Oberfläche akzeptiert nur Pakete, die mit dem Release-Schlüssel dieses
Projekts signiert sind. Jedes Medien-Update, ob über das Installationsprogramm
oder über dieses Formular, schreibt dieselbe Partition neu wie die
Erstinstallation; die obige Warnung vor einem Stromausfall gilt daher auch
hier – ebenso für einen Reset der Platine selbst. Da ein Image Dateien des
Herstellers enthält, wird keines veröffentlicht; das Formular ist daher für den
Inhaber des Schlüssels gedacht oder für alle, die die Plattform mit einem
eigenen Schlüssel erstellen.

**SD-Karte:** Im Fallback-Startmodus der Medienschicht kann der Daemon statt
aus der Anwendungspartition von einer microSD-Karte laufen. Nur das Programm
des Herstellers bindet die Karte ein; daher funktioniert das nur in diesem
Fallback-Modus. Es ist kein Weg, einen der obigen Schritte zu umgehen.

Das Format eines Bundles ist unter
[Release-Bundles für die Kamera](../../../media/docs/hardware.md#camera-release-bundles)
beschrieben.
