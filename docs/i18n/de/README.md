<!-- Translated from README.md. Authoritative version: ../../../README.md -->

# Lokale Firmware für die JOOAN W3-U

<p align="center">
  <img src="../../../docs/images/w3u-camera.jpg" width="320"
       alt="Die Kamera JOOAN W3-U in der Hand: zwei übereinander angeordnete Objektive in einem weißen Schwenk-Neige-Gehäuse, zwei Wi-Fi-Antennen und ein kurzes Ethernet-Anschlusskabel">
  <br>
  <sub>Die Zielkamera dieses Projekts: JOOAN W3-U (<code>JA-A12</code>) mit zwei Objektiven.</sub>
</p>

Lokal ausgerichtete Firmware für genau ein Kameramodell: die JOOAN W3-U
(`JA-A12`). Sie trennt die Kamera von der JOOAN-Cloud und den P2P-Diensten und
ersetzt diese durch lokale, authentifizierte Dienste: eine Webkonsole, SSH,
RTSP-Video sowie Schwenken/Neigen und Presets per ONVIF für einen NVR wie
Frigate.

Dies ist keine vollständige Open-Source-Firmware. Bootloader, Linux-Kernel 3.10,
Kernelmodule, Sensorabstimmung und Funkmodul-Firmware des Herstellers bleiben
erhalten, ebenso das Kameraprogramm `jooanipc` des Herstellers als Fallback.
Darüber werden zwei Schichten installiert:

- die **Plattform** (das Stammverzeichnis dieses Repositorys) wird in die
  beschreibbare Partition `/opt` installiert. Sie stellt Webkonsole und API,
  SSH, Wi-Fi, Uhrzeit und signierte Updates bereit und hält die Kamera vom
  Internet fern;
- der **Medien-Daemon** [jooanipc-open](../../../media/) wird in die
  Anwendungspartition installiert. Er stellt Video, ONVIF und Schwenken/Neigen
  bereit, sodass das Programm des Herstellers bei einem normalen (nativen)
  Start nie ausgeführt wird.

[Architektur](../../../docs/ARCHITECTURE.md) erläutert die Startmodi und die
Vertrauensgrenze.

<p align="center">
  <img src="../../../docs/images/webui-overview.png" width="480"
       alt="Die Seite „Übersicht“ der lokalen Webkonsole: beide Objektivansichten (hier weichgezeichnet) sowie Schnellaktionen für Nachtsicht, Weißlicht, Privatmodus, Sprechen und Sirene">
  <br>
  <sub>Die lokale Webkonsole (beide Objektivansichten weichgezeichnet). Sie läuft auf der Kamera,
  braucht keine Cloud und ist in sieben Sprachen verfügbar. Weitere Ansichten folgen weiter unten.</sub>
</p>

## Status

- **Technische Vorabversion; noch wird nichts unterstützt.** Releases werden an
  einer einzigen physischen Kamera getestet. Ein Release gilt erst dann als
  unterstützt, wenn auf dieser Kamera die Prüfungen zu signierter Installation
  und Deinstallation, Migration, Wi-Fi, Netzwerkzeit, SSH, Routing, Stromausfall
  und Wiederherstellung nach einem Kaltstart bestanden sind
  ([Hardware-Freigabekriterien](../../../tests/TEST_PLAN.md#hardware-promotion-gates)).
- **Veröffentlicht: v0.2.39** (2026-11-07): das Plattformpaket und erstmals auch
  die Binärdateien und Skripte der Medienschicht. Das Image für die Kamera
  erstellt weiterhin jeder Besitzer selbst aus der App-Partition genau dieser
  Kamera, da die Partition Dateien des Herstellers enthält; deshalb wird kein
  Image veröffentlicht.
- **Die gesamte Kette ist jetzt einmal vollständig auf einer echten Kamera
  durchgelaufen.** Eine Testkamera wurde auf die Werksfirmware zurückgesetzt,
  ohne Hersteller-App ins Wi-Fi-Netzwerk eingebunden, drahtlos (OTA) auf die
  Plattform aktualisiert und mit einem Medien-Image geflasht, das aus ihrer
  eigenen Partition erstellt wurde; sie lieferte Video über RTSP und überstand
  mit dem offenen Software-Stack einen Kaltstart (siehe
  [den Bericht zum Gesamtdurchlauf](../../../docs/reports/hardware-roundtrip-20261108.md) und
  [den Abschnitt zur Medienschicht](./INSTALL.md#the-media-layer)). Da dies erst
  ein einziges Mal gelungen ist, ist der Ablauf weiterhin als Vorabversion zu
  betrachten: unbedingt die vollständige Sicherung durch das
  Installationsprogramm erstellen lassen und die Kamera während des Flashens
  nicht vom Strom trennen.
- **v0.2.21** (2026-10-16, nur Plattform) bleibt veröffentlicht. Es ist das
  letzte Release, das auf einer Kamera mit Werksfirmware installiert wurde.
  Damit stammt das Video weiterhin vom Programm des Herstellers, das abgeschottet
  läuft.

## Exakte Zielhardware

Dieses Projekt ist ausschließlich für Geräte bestimmt, die alle folgenden
Merkmale erfüllen:

- von der OEM-Firmware gemeldetes Produkt/Modell: `JA-A12` / Paketkennung `A12`;
- Platine: `JA-6621 V1.0`;
- SoC: Ingenic T23N;
- Bildsensoren: zwei, `cv2005` und `cv2005s1`;
- Funkmodul: SeekWave SKW6316 über USB (`SV6160LITE` beim verifizierten Gerät);
- Flash-Speicher: 8 MiB SPI-NOR (verifiziertes Gerät: Puya P25Q64HA).

Auf der Verkaufsverpackung des verifizierten Geräts steht: Marke `JOOAN`, Modell
`W3-U`, FCC ID `2BBQ4-W3U`, 5 V / 1,5 A. Daraus geht nur der Produktname hervor,
und der allein reicht nicht aus: Über die Kompatibilität entscheidet die
Checkliste oben. Das Thingino-Image namens „Jooan W3-U“ ist für eine andere
Revision mit SC2336P/ATBM6132U bestimmt und nicht austauschbar. Produktname und
Gehäuseform sind kein Nachweis der Kompatibilität. Vor jeder Installation
unbedingt [Kompatibilität](../../../docs/COMPATIBILITY.md) lesen.

## Was funktioniert und was nicht

Mit 0.2.39 und der zugehörigen Medienschicht:

- **Funktioniert:** RTSP für beide Objektive (1920×1080 und 1024×576 bei 15 fps,
  H.264); Schwenken/Neigen per ONVIF und 16 benannte Presets für Frigate;
  Referenzfahrt bei jedem Start; Spiegeln/Drehen des Bildes, manuelle
  Nacht-/IR-Cut-Umschaltung und Weißlicht; eine einzige Admin-Oberfläche unter
  `https://<camera>/` für Smartphones und Computer (`http://<camera>/` leitet
  dorthin weiter); SSH; Wi-Fi mit Netzwerkliste, statischer Adresse sowie
  Bereitstellen, Übernehmen und Rollback; Neustart, täglicher Neustart und
  Zurücksetzen auf Standardwerte; Privatmodus; Patrouille zwischen Presets;
  exakte ONVIF-Bewegungen; optionale Netzwerkzeit von einem lokalen Server;
  mDNS; signierte Updates.
- **Eingeschaltet, außer bei `audio=0`:** Audio (Mikrofon über RTSP, Sprechen
  über den Lautsprecher aus Frigate, der Admin-Oberfläche oder der API).
- **Fehlt:** Bewegungs- und Personenerkennung; Digitalzoom; ein Logo im Bild;
  Aufzeichnung auf der Kamera (im qualifizierten Profil deaktiviert: zum
  Aufzeichnen den NVR verwenden) und SD-Karten-Verwaltung; jeglicher Zugriff
  über Cloud oder Smartphone-App.

[OEM-Vergleich](../../../docs/OEM-COMPARISON.md) vergleicht diese Firmware Punkt
für Punkt mit der Firmware des Herstellers.

<p align="center">
  <img src="../../../docs/images/webui-system.png" width="480"
       alt="Die Seite „System“ der Webkonsole: installiert sind Plattform 0.2.39 und Medien 0.1.0, dazu Bedienelemente für Update, Uhrzeit, Passwort, SSH-Schlüssel und Neustart">
  <br>
  <sub>Die Seite „System“: Plattform- und Medienversion, Updates, Uhrzeit, Passwörter,
  SSH-Schlüssel und Zurücksetzen – alles auf der Kamera, ohne Cloud.</sub>
</p>

## Installation

Die Anleitung steht unter [Installation](./INSTALL.md). Die Erstinstallation
erfolgt über Wi-Fi mithilfe der Update-Funktion der Herstellerfirmware selbst;
spätere Updates laufen über die Webkonsole. Es wird weder eine SD-Karte noch ein
Kabel oder Flash-Programmiergerät benötigt, und die Kamera bleibt geschlossen.
Ein Flash-Programmiergerät ist nur nötig, um eine Kamera wiederherzustellen, die
nicht mehr startet ([Wiederherstellung](./RECOVERY.md)).

Für eine geführte Installation ohne Terminal bündelt das
[Installationsprogramm](../../../installer/) den gesamten Ablauf – Einrichtung,
Plattform-Update, Sicherung, Image-Erstellung und Flashen – in einem Fenster.
Es startet im Probelauf, der alles plant und nichts verändert, zeigt ein
Live-Protokoll an und fragt nur einmal nach: vor dem einzigen unumkehrbaren
Schritt.

<p align="center">
  <img src="../../../docs/images/gui-running.png" width="480"
       alt="Das Fenster des Installationsprogramms: ein Formular für Kameraadresse, Pakete und Schlüssel, ein Kontrollkästchen „Dry run“ (Probelauf) und ein farblich markiertes Live-Protokoll der fünf Installationsschritte">
  <br>
  <sub>Das Installationsprogramm mit nur einem Fenster (<code>python3 installer/gui.py</code>) beim Durchlaufen der fünf Schritte.</sub>
</p>

## Sicherheit in Kürze

- Die Kamera muss in einem Netzwerk betrieben werden, das ihren Internetzugang
  sperrt. Bei jedem Start läuft für einige Sekunden der Webserver des
  Herstellers, bevor diese Firmware übernimmt. Die Firmware entfernt die
  Standardrouten der Kamera, hat aber keinen Paketfilter; die eigentliche
  Schutzgrenze ist daher die Isolation durch den Router.
- Webkonsole und API laufen über unverschlüsseltes HTTP auf Port 80. Eine
  Anfrage muss die Kamera über eine private oder Loopback-IP-Adresse oder über
  ihren eigenen Namen ansprechen (standardmäßig `jooan-w3u`, allein oder mit der
  Endung `.local`, `.lan`, `.home.arpa` oder `.internal`). Alle anderen Anfragen
  erhalten `403 host_rejected`.
- Die Anmeldung erfolgt mit dem Benutzer `admin` und dem Anfangspasswort
  `admin`. Dieses bleibt gültig, bis es auf der Seite „System“ geändert wird,
  und SSH verwendet dasselbe Passwort. Der Medien-Daemon hat eigene
  Zugangsdaten, ein eigenes TLS-Zertifikat und eine Drosselung fehlgeschlagener
  Anmeldeversuche.
- `telnetd` und der Webserver des Herstellers werden im Sekundentakt beendet.
  Läuft das Programm des Herstellers (nur in den Fallback-Modi), blockiert ein
  Wächterprozess dessen Cloud-Verkehr und beschränkt seine Listener auf
  Loopback.

Details: [Netzwerksicherheit](../../../docs/NETWORK-SECURITY.md) und
[Sicherheitsrichtlinie](../../../SECURITY.md), die auch beschreibt, wie sich
eine Sicherheitslücke melden lässt.

## Aufbau des Repositorys

| Pfad | Inhalt | Lizenz |
| --- | --- | --- |
| `build.sh`, `build-uninstall.sh`, `release.sh` | Einstiegspunkte für Releases ([Erstellen](../../../docs/BUILDING.md)) | GPL-2.0-only |
| `packaging/` | Manifest für das exakte Zielgerät und Zusammenbau der IronMan-Pakete | GPL-2.0-only |
| `runtime/` | Boot-Hook, Supervisor, Slot- und Wartungsskripte | GPL-2.0-only |
| `src/` | `joan-daemon` (Webkonsole und API), der Abschottungs-Wächter, Prüfwerkzeuge für Releases, Paketwerkzeuge | GPL-2.0-only |
| `web/` | die Webkonsole | GPL-2.0-only |
| `tools/` | `upload_ota.py` (Erstinstallation), `make-github-release.py`, Abruf der Toolchain, Wi-Fi-Diagnose | GPL-2.0-only |
| `tests/`, `ci/` | hostseitige Validierung | GPL-2.0-only |
| `docs/` | Dokumentation | GPL-2.0-only |
| [`media/`](../../../media/) | Medien-Daemon `jooanipc-open`, seine Boot-Skripte, appfs-Werkzeuge und Installationsprogramm für Release-Bundles | MIT |

Die `libimp.so`, die der Medien-Daemon lädt, wird aus dem OpenIMP-Fork gebaut,
Branch [`jooan/oem-t23-abi`](https://github.com/ADCDS/openimp/tree/jooan/oem-t23-abi);
dieser Fork ist ein separates Repository.

Das Repository und seine Releases enthalten keine werkseitigen Geheimnisse,
Wi-Fi-Zugangsdaten, Geräteschlüssel, Flash-Dumps, OEM-Firmware oder sonstigen
Binärdateien des Herstellers. Wird eine beibehaltene OEM-Komponente benötigt,
stammt sie von der eigenen Kamera des Besitzers. Ein Bundle der Medienschicht
enthält ein appfs-Image, das aus dieser Kamera neu erstellt wurde, und bleibt
daher privat.

## Dokumentation

- [Installation](./INSTALL.md): Installation eines veröffentlichten Releases.
- [Kompatibilität](../../../docs/COMPATIBILITY.md): wie sich feststellen lässt, ob
  die eigene Kamera das unterstützte Modell ist.
- [OEM-Vergleich](../../../docs/OEM-COMPARISON.md): was dieser Firmware im
  Vergleich zur Herstellerfirmware fehlt.
- [Wiederherstellung](./RECOVERY.md): Deinstallation, Rollback und
  SPI-Wiederherstellung.
- [Netzwerksicherheit](../../../docs/NETWORK-SECURITY.md): Listener,
  Host-Prüfungen, Wi-Fi, Isolation.
- [Architektur](../../../docs/ARCHITECTURE.md): Startmodi, beibehaltene
  Komponenten, Funktionsabdeckung.
- [API](../../../docs/API.md): `/api/v1/` der Plattform.
- [Erstellen und Veröffentlichen](../../../docs/BUILDING.md): Toolchain,
  Signierung, Größenbudget, Release-Bundles, Qualifizierungs-Checkliste.
- [Medien-Daemon](../../../media/README.md) und
  [Hardware-Integration](../../../media/docs/hardware.md).
- [Testplan](../../../tests/TEST_PLAN.md),
  [Komponenten von Drittanbietern](../../../docs/THIRD_PARTY.md),
  [Sicherheitsrichtlinie](../../../SECURITY.md).

## Lizenz

Die Plattform (alles außerhalb von `media/`) steht unter der Lizenz
[GPL-2.0-only](../../../LICENSE); `media/` steht unter MIT
([media/LICENSE](../../../media/LICENSE)). Jede Datei trägt ihren eigenen
SPDX-Header und behält ihre eigene Lizenz, auch dort, wo ein MIT-Skript zur
Laufzeit ein GPL-Skript einbindet. Die Plattform linkt die kameraeigenen
Bibliotheken von Mbed TLS unter einer zusätzlichen Erlaubnis, deren genauer
Wortlaut unter
[Komponenten von Drittanbietern](../../../docs/THIRD_PARTY.md#mbed-tls-linking-exception)
steht. Für OEM- oder Drittanbieter-Artefakte, die nicht im Repository enthalten
sind, wird keine Lizenz erteilt.
