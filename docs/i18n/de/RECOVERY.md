<!-- Translated from docs/RECOVERY.md. Authoritative version: ../../RECOVERY.md -->

# Wiederherstellung

## Stufen der Wiederherstellung

Es werden zwei voneinander unabhängige Wege benötigt:

- **Rollback/Deinstallation per Paket** für eine Kamera, die noch startet und
  deren lokale Verwaltung oder Update-Weg der OEM-Firmware noch funktioniert.
- **Externe SPI-Wiederherstellung** für eine Kamera, die nicht mehr startet
  oder nicht erreichbar ist.

Das Neuflashen der appfs durch die Medienschicht ergänzt einen dritten,
manuellen Weg für den einen Fall, in dem es die Kamera mit einer beschädigten
Partition weiterlaufen lässt; siehe
[Wiederherstellung beim Neuflashen der appfs](#appfs-reflash-recovery).

Ein Deinstallationspaket, das nur auf der Kamera gespeichert ist, ist kein
unabhängiges Wiederherstellungsmittel. Bewahren Sie es außerhalb des Geräts
auf. Die externe SPI-Wiederherstellung bleibt der zuverlässigste Weg für eine
Kamera, die weder starten noch ein Update annehmen kann.

## Material für die Wiederherstellung

Bewahren Sie Folgendes offline auf und kennzeichnen Sie es als zugehörig zu
genau einer physischen Kamera:

- zwei byteidentische vollständige Auslesungen des Flash-Speichers (je 8 MiB)
  samt ihrer SHA-256-Hashes, erstellt über SSH, sobald die Plattform läuft
  ([Installation, Schritt 7](./INSTALL.md#7-back-up-the-camera-over-ssh)), oder
  mit einem SPI-Programmiergerät;
- Fotos der Platine `JA-6621 V1.0`, der Ausrichtung des Flash-Chips und der
  Klemme sowie der Verkabelung;
- Modell des Programmiergeräts, Spannungseinstellung, gewählter Flash-Baustein
  und ein nachweislich funktionierender Lesebefehl;
- die exakten Installations- und Deinstallationspakete, Manifeste und Hashes;
- eine Auslesung der appfs-Partition (`/dev/mtd4`), erstellt vor jedem Update
  der Medienschicht; das Verzeichnis `backups/` des Bundles enthält nur
  Konfigurationsdateien;
- eine Kopie des Release-Quellcodes und der Toolchain-Metadaten;
- um vertrauliche Angaben bereinigte Notizen zum letzten erfolgreichen Start
  und zum Netzwerkzustand.

Geben Sie den Dump einer Kamera niemals als Wiederherstellungs-Image für ein
anderes Gerät weiter.

<a id="booting-camera-rollback"></a>
## Rollback bei einer Kamera, die noch startet

1. Bringen Sie Kamera und Arbeitsrechner in das isolierte
   Wiederherstellungsnetzwerk.
2. Sperren Sie im Router jeglichen Internetzugang der Kamera.
3. Prüfen Sie den Hash des Deinstallationspakets und die Zielmetadaten für
   `A12`.
4. Verwenden Sie den authentifizierten lokalen Update-Weg oder den
   dokumentierten Offline-Übertragungsweg.
5. Halten Sie die Stromversorgung stabil, bis das Paket den Abschluss meldet
   und der Neustart beendet ist.
6. Führen Sie einen Kaltstart durch, prüfen Sie Status und Listener und
   vergewissern Sie sich, dass der beabsichtigte wiederhergestellte Zustand
   vorliegt.

Die Deinstallation erhält die Werksdaten und die von der OEM-Firmware
gespeicherte Wi-Fi-Konfiguration, löscht aber den Zustand der Plattform,
einschließlich des übernommenen Wi-Fi-Netzwerks (`wifi.json`); danach
verwendet die OEM-Anwendung ihr eigenes gespeichertes Netzwerk, das nicht
unbedingt dasjenige ist, das die Kamera zuletzt verwendet hat. Wenn die
Authentifizierung an der Kamera fehlschlägt, die Kamera das Paket nicht
validieren kann oder wiederholt neu startet, versuchen Sie keine weiteren
Updates über das Netzwerk, sondern nutzen Sie die externe Wiederherstellung.

Das Ersetzen der Runtime (Laufzeitumgebung) ist eine Wartungstransaktion: Der
signierte Controller und das SSH-Wiederherstellungsbundle bleiben verfügbar,
während die einzige persistente Runtime ersetzt wird. Während der
zweiminütigen Funktionsprüfung aktiviert der Supervisor das übernommene
Wi-Fi-Netzwerk (ein nativer Start, für den es nichts abzuwarten gibt, gibt das
Warten auf sein Gateway nach 300 s auf) und startet einen Kandidaten neu,
dessen Start fehlgeschlagen ist: `start.sh` ist auf 15 s begrenzt, während
`entropy-ready.sh` bis zu 30 s wartet, und in der Frühphase eines nativen
Starts sammelt sich Entropie nur langsam. Schlägt der Kandidat dennoch fehl
oder wird die Funktionsprüfung unterbrochen, bleibt er als einzige Runtime
ausgewählt, weil das Update die vorherige bereits gelöscht hat, und der
Supervisor startet ihn immer wieder neu; nutzen Sie dann zur Wiederherstellung
das vom Controller bereitgestellte SSH und versuchen Sie das Update erneut oder
deinstallieren Sie die Plattform. (Bevor dies behoben wurde, entfernte eine
fehlgeschlagene Funktionsprüfung den Kandidaten und hinterließ keine Runtime;
der folgende Absatz beschreibt diesen Zustand.) Die alte Runtime läuft unter
Umständen bis zum Neustart nach dem Update aus dem tmpfs weiter, ist aber kein
Rollback-Slot im Flash-Speicher. Ein fehlgeschlagenes Plattform-Update fällt
nicht auf die Update-Funktion des Herstellers zurück: GoAhead wird weiterhin
beendet, solange der Controller läuft.

Ohne Runtime gibt es nichts, was das übernommene Wi-Fi-Netzwerk aktiviert. Ein
nativer Start erreicht dann sein Gateway nicht, erkennt, dass auch nichts
versucht, es zu erreichen, schlägt nach 300 s fehl und setzt dauerhaft
`native-failed`; spätere Starts führen die abgeschottete OEM-Anwendung aus, die
ihrem eigenen gespeicherten Netzwerk beitritt. Beim verifizierten Gerät handelt
es sich dabei um das Werks-Testnetzwerk des Herstellers, `JOOANTEST` (offen;
die Kamera sucht es aktiv als versteckte SSID). So erreichen Sie die Kamera in
diesem Zustand:

1. Stellen Sie auf einem Access Point in Funkreichweite der Kamera vorübergehend
   ein verstecktes, offenes Netzwerk `JOOANTEST` bereit, beschränkt auf die
   MAC-Adresse der Kamera und in einem isolierten Subnetz. Die Kamera tritt
   innerhalb von Sekunden bei. Möglicherweise bezieht sie keine IPv4-Adresse,
   doch ihre IPv6-Link-Local-Adresse antwortet auf SSH (dropbear lauscht auf
   `[::]:22`), zum Beispiel über `ssh -L 2222:[fe80::…%<bridge>]:22` auf diesem
   Access Point.
2. Installieren Sie die Runtime als stabilen Slot: Kopieren Sie `runtime.tar.gz`
   und `runtime.md5` aus dem Stage-Verzeichnis des Releases nach
   `/opt/custom/jooan-local/slots/A/` und führen Sie dann, nachdem Sie
   `common.sh` in die Shell eingelesen haben,
   `jl_lock; jl_write_selection A - 0; jl_unlock` aus.
3. Löschen Sie `/opt/jooanipc-open/native-failed`, führen Sie `sync` aus und
   starten Sie neu. Der Supervisor startet die Runtime (und startet sie erneut,
   falls ihr erster Start fehlschlägt), ihr Hook aktiviert das übernommene
   Netzwerk, und der native Start qualifiziert sich.
4. Entfernen Sie das temporäre Netzwerk.

## Ein abgebrochenes Update

Ein Plattform-Update auf einer Kamera mit knappem Speicherplatz löscht die
einzige Runtime im Flash-Speicher, bevor es die neue schreibt. Bricht es
dazwischen ab, arbeitet die Kamera aus dem RAM weiter, doch im Flash-Speicher
liegt nichts, was beim nächsten Mal gestartet werden könnte: Der Bereich
„System“ meldet, dass das Update abgebrochen wurde, und ein Neustart wird mit
`no-runtime` abgelehnt.

**Starten Sie die Kamera vorerst keinesfalls neu und trennen Sie sie nicht vom
Strom.** Der nächste Start hätte kein Netzwerk, und Sie wären auf die oben
beschriebenen Schritte angewiesen. Über SSH:

```sh
cat /opt/custom/jooan-local/state/selection
ls /opt/custom/jooan-local/slots/
```

Nennt die Auswahl einen Slot (`A - 0`), ist dieser Slot vorhanden und zeigt
`df -k /opt` mindestens 51 KiB frei an, ist nichts verloren. Sind beim Start
weniger als 50 KiB frei, löscht das Skript `startapp` des Herstellers
`/opt/etc` und `/opt/jooanipc-open` – das ist schlimmer, als keine Runtime zu
haben; schaffen Sie daher zuerst Platz. Gelöscht werden darf: alles mit dem
Namen `*.new`, `*.next` oder `*.previous` unter `/opt/custom/jooan-local` und
`/opt/jooanipc-open` sowie `state/net-loss.log`. Keinesfalls gelöscht werden
darf: `config/`, `state/selection`, `controller.tar.gz`, `recovery/`, ein
ausgewählter Slot sowie in `/opt/jooanipc-open` die Dateien `camera.ini` und
`deployment.env`, die beiden Boot-Skripte und die Schlüssel. Lautet die Auswahl
`- - 0`, spielen Sie wieder eine Runtime ein. Beenden Sie zuvor jedes
Installationsprogramm, das noch auf diese Kamera wartet: Es würde fortfahren,
sobald es einen Neustart bemerkt.

1. Suchen Sie das fehlgeschlagene Paket; es bleibt bis zu einem Neustart im
   RAM:
   ```sh
   ls /run/jooan-local/firmware.*.sqfs
   mkdir /tmp/fw && mount -t squashfs -o ro,loop /run/jooan-local/firmware.<id>.sqfs /tmp/fw
   ```
   Ist es nicht mehr vorhanden, liegen dieselben beiden Dateien in der Datei
   `JOOAN_FW_PKG` des Releases. Führen Sie auf Ihrem Computer
   `dd if=JOOAN_FW_PKG bs=96 skip=1 of=p.sqfs && unsquashfs p.sqfs runtime.tar.gz runtime.md5`
   aus, dann auf der Kamera `mkdir -p /tmp/fw`, und kopieren Sie die Dateien
   dorthin, etwa mit
   `ssh admin@CAMERA 'cat > /tmp/fw/runtime.tar.gz' < squashfs-root/runtime.tar.gz`.
2. Kopieren Sie die Runtime in einen Slot und lassen Sie die Prüfsumme
   entscheiden. Prüfen Sie zuvor, ob `df -k /opt` mindestens die Größe von
   `runtime.tar.gz` plus 60 KiB als frei anzeigt; andernfalls schaffen Sie aus
   dem oben genannten Grund Platz.
   ```sh
   cd /opt/custom/jooan-local/slots
   rm -rf A.new
   mkdir A.new && cp /tmp/fw/runtime.tar.gz /tmp/fw/runtime.md5 A.new/
   [ "$(md5sum A.new/runtime.tar.gz | cut -c1-32)" = "$(cat A.new/runtime.md5)" ] && sync && mv A.new A && sync
   ```
3. Wählen Sie den Slot aus, und zwar mit den Funktionen des laufenden
   Controllers selbst:
   ```sh
   . /run/jooan-local/controller/boot/common.sh; jl_lock; jl_write_selection A - 0; jl_unlock
   ```

Der Supervisor liest die Auswahl nur beim Start; das stört also nichts. Zeigt
`df -k /opt` 51 KiB oder mehr frei an, kann die Kamera jetzt gefahrlos neu
starten, und das Update lässt sich erneut versuchen. Die Admin-Oberfläche hält
Neustart und Zurücksetzen gesperrt, bis die Kamera neu gestartet ist; starten
Sie sie daher aus derselben SSH-Sitzung mit `jl_reboot_asked` neu. Dieser
Befehl sichert zuerst die Uhrzeit und teilt dem nächsten Start mit, dass der
Neustart angefordert wurde. So wurde es am 2026-11-07 auf camera1 tatsächlich
durchgeführt, aus dem RAM heraus.

<a id="a-media-change-that-stops-native-boot"></a>
## Eine Medienänderung, die den nativen Start verhindert

Beendet sich der Medien-Daemon während eines nativen Starts, setzt sein
scharfgeschalteter Watchdog die Platine zurück. Hat ein Start den Fehler
verzeichnet, setzt der nächste dauerhaft `native-failed` und startet über den
Handoff (den Fallback-Start); ein Start, der lediglich vorzeitig abgebrochen
wurde, durch einen Stromausfall oder einen Systemhänger, erhält vorher noch
zwei weitere Versuche. Ein Handoff, der ebenfalls fehlschlägt, setzt dauerhaft
`boot-trial` und lässt die abgeschottete OEM-Anwendung weiterlaufen. Ein von
der Plattform angeforderter Neustart wird dem Start, den er unterbrochen hat,
nicht angerechnet; einen zuvor verzeichneten Fehler hebt er jedoch nicht auf.

Ein nativer Start ohne Gateway wartet – ohne Reset und ohne Markierung –,
solange daraus noch etwas werden kann: Es gibt ein übernommenes Netzwerk, eine
Runtime versucht, ihm beizutreten, und ein früherer Start hat sich auf dieser
Kamera bereits qualifiziert. Andernfalls schlägt er wie oben beschrieben nach
300 s fehl. Ein Release, das das Gateway erst nach dem Start verliert,
qualifiziert sich dennoch, es sei denn, das geschieht bei drei Starts in Folge.
Ist ein übernommenes Netzwerk endgültig verschwunden und hat sich die Kamera
zuvor bereits qualifiziert, wartet sie weiter: Stellen Sie wieder ein Netzwerk
mit diesem Namen und Passwort bereit, um sie zu erreichen. Die Plattform und
SSH antworten weiterhin, die HTTPS-Oberfläche hingegen nicht. So stellen Sie
die Kamera wieder her:

1. Lesen Sie die Fehlerursache in `/tmp/jooanipc-open.log` nach oder, falls die
   Platine seitdem zurückgesetzt wurde, in einem auf einen anderen Rechner
   gespiegelten Protokoll. Suchen Sie mit grep nach `startup failed` und
   `health check` sowie in den Kernelmeldungen nach `Process `: Ein Oops
   bedeutet, dass die Ursache möglicherweise nicht beim Release liegt.
2. Machen Sie die Ursache rückgängig, sofern es sich um eine gespeicherte
   Einstellung handelt, zum Beispiel in
   `/opt/jooanipc-open/state/controls.json` oder
   `/opt/jooanipc-open/camera.ini`.
3. Führen Sie
   `cd /opt/jooanipc-open && rm -f native-failed native-trial native-retry boot-trial && sync`
   aus und starten Sie neu. Der Start erfolgt wieder nativ, und das
   Installationsprogramm akzeptiert die Kamera.

Dies ist auf der Testkamera am 2026-11-06 zweimal passiert, mit einem Release,
das bei eingeschaltetem Privatmodus nicht starten konnte. Die Kamera wurde
jedes Mal auf diese Weise in etwa 5 Minuten wiederhergestellt.

Ein drittes Mal geschah es am selben Tag mit einem fehlerfreien Release:
`IMP_ISP_Open failed (-1)`. Ein Überwachungsskript hatte `/proc/jz/isp/isp-m0`
über SSH gelesen, während die Platine startete. Wird diese Datei gelesen,
nachdem das ISP-Modul geladen wurde, aber bevor der Daemon den ISP geöffnet
hat, löst sie im Treiber einen Oops aus (`Process awk` in den Kernelmeldungen);
das Öffnen durch den Daemon eine Sekunde später schlug daraufhin fehl. Ein
nativer Start befindet sich 30 s oder länger in diesem Zustand, während er auf
das Wi-Fi wartet. Alles, was diese Datei ausliest, darf das erst tun, nachdem
`/api/v1/status` den Wert `"ready":true` meldet.

Die signierte Deinstallation ist ein destruktives Entfernen, keine
Wiederherstellung des vorherigen Zustands. Sie entfernt die Webkonsole, die
Routenbereinigung, den Wächterprozess, das SSH des Projekts sowie Controller
und Runtime in komprimierter Form; beim nächsten Start kehrt die Kamera zum
Verhalten der OEM-Firmware mit GoAhead zurück, und die OEM-Anwendung läuft ohne
Abschottung. Die Dateien der Medienschicht in `/opt/jooanipc-open` und ihr
Release in der appfs bleiben im Flash-Speicher, doch nichts startet sie mehr,
sobald `local.rc` entfernt ist. Die Deinstallation stellt weder den alten,
nicht authentifizierten Telnet-Hook noch eine archivierte Vorgängerversion von
`/opt/etc/local.rc` wieder her. Halten Sie die Isolation durch den Router vor,
während und nach der Deinstallation aufrecht.

<a id="appfs-reflash-recovery"></a>
## Wiederherstellung beim Neuflashen der appfs

`media/scripts/upgrade-appfs.sh` flasht `/dev/mtd4` neu, die appfs
(Anwendungspartition): Sie enthält das `startapp` des Herstellers, `jooanipc`,
Kernelmodule, die Sensorabstimmung (IQ) und das Medien-Release. Bevor das
Skript den Medien-Daemon anhält, setzt es die Wartungssperre der Plattform
(`/run/jooan-local/state.lock`), die die Runtime-Neustarts durch den Supervisor
und den Neustart durch den Netzwerk-Watchdog aussetzt, und ein
`watchdog`-Feeder aus dem rootfs übernimmt das Bedienen des scharfgeschalteten
Hardware-Watchdogs. Der Bereich System → Media der Admin-Oberfläche führt genau
dieses Skript aus. `media/scripts/first-appfs.sh` für das erste Medien-Release
einer Kamera durchläuft dieselben Schritte, ausgehend von der Werksanwendung.
Sein Rollback entfernt zusätzlich die Medienkonfiguration, sodass die Kamera in
den Werkszustand zurückkehrt und nur die Plattform behält.

- **Vor dem Commit-Punkt** (der Umschaltung der Konfiguration unmittelbar vor
  `flashcp`) führt jeder Fehler zu einem Neustart in die unveränderte appfs.
- **Danach** wird ein Flash-Vorgang, der die Verifizierung nicht besteht, aus
  einer RAM-Kopie der ursprünglichen appfs zurückgerollt, mit bis zu drei
  Versuchen; anschließend werden die Konfigurationsdateien und
  `native-boot.sh` wiederhergestellt, und die Kamera startet neu.
- **Schlägt auch das Rollback fehl**, startet das Installationsprogramm nicht
  neu. Die Kamera bleibt in Betrieb, der rootfs-Feeder läuft weiter, die Sperre
  bleibt gesetzt, und beide Images liegen in `/tmp`:
  `/tmp/jipc-appfs-rollback.bin` (die ursprüngliche appfs) und
  `/tmp/jipc-flash-candidate.squashfs` (das neue Image). SSH funktioniert
  weiterhin, weil die Plattform aus `/run` und dem rootfs läuft.

Stellen Sie die Kamera aus diesem Zustand über SSH wieder her, ohne neu zu
starten:

```sh
flashcp -v /tmp/jipc-appfs-rollback.bin /dev/mtd4
md5sum /dev/mtd4      # must equal the original appfs MD5
cp /tmp/jipc-install/boot-handoff.sh.before /opt/jooanipc-open/boot-handoff.sh
cp /tmp/jipc-install/native-boot.sh.before /opt/jooanipc-open/native-boot.sh
cp /tmp/jipc-install/camera.ini.before /opt/jooanipc-open/camera.ini
cp /tmp/jipc-install/deployment.env.before /opt/jooanipc-open/deployment.env
chmod 700 /opt/jooanipc-open/boot-handoff.sh /opt/jooanipc-open/native-boot.sh
chmod 600 /opt/jooanipc-open/camera.ini /opt/jooanipc-open/deployment.env
sync
reboot
```

Eine Erstinstallation legt keine `.before`-Dateien an, weil es noch keine
Medienschicht gab. Führen Sie nach dem `flashcp` und der Prüfung anstelle der
drei `cp`-Zeilen Folgendes aus:

```sh
cd /opt/jooanipc-open && rm -f boot-handoff.sh camera.ini deployment.env enabled native
```

Die MD5-Summe der ursprünglichen appfs ist das zweite Argument, das dem
Installationsprogramm übergeben wurde: der Wert `expected_current_appfs_md5`
aus dem Bundle, den `install-camera.py` auch bei der Vorabprüfung ausgibt.
Wiederholen Sie `flashcp`, bis `md5sum` übereinstimmt; starten Sie vorher
keinesfalls neu. `/tmp` liegt im RAM: Bei jedem Neustart gehen die Images
verloren.

Fällt der Strom aus oder startet die Kamera neu, während die appfs beschädigt
ist, fehlt `startapp`: Nichts bindet `local.rc` ein, sodass weder die Plattform
noch SSH startet. Die Wiederherstellung ist dann nur noch mit einem
SPI-Flash-Programmiergerät (8 MiB SPI NOR) und einer Sicherung der
appfs-Partition oder des gesamten Flash-Speichers möglich, wie unten
beschrieben.

<a id="external-spi-recovery"></a>
## Externe SPI-Wiederherstellung

Die verifizierte Platine verwendet einen 8 MiB großen SPI NOR (beim
untersuchten Gerät ein Puya P25Q64HA). Überprüfen Sie, welcher Chip tatsächlich
bestückt ist, statt davon auszugehen.

1. Trennen Sie die Kamera vom Strom und vom Ethernet.
2. Verwenden Sie ein für 3,3 V geeignetes Programmiergerät und prüfen Sie Pin 1
   und die Ausrichtung anhand der Platine und des Datenblatts des Flash-Chips.
3. Lesen Sie den Chip vor dem Schreiben noch einmal aus und bewahren Sie dieses
   Image des fehlerhaften Zustands auf.
4. Vergewissern Sie sich, dass die Sicherung genau 8 MiB groß ist und ihr
   SHA-256-Hash mit dem einer der vor der Installation erstellten Auslesungen
   übereinstimmt.
5. Schreiben Sie das vollständige Image mit dem verifizierten Befehl für das
   Programmiergerät.
6. Lesen Sie den Chip zurück und vergleichen Sie jedes Byte mit der Sicherung.
7. Trennen Sie das Programmiergerät, bevor Sie die Kamera wieder regulär mit
   Strom versorgen.
8. Starten Sie die Kamera nur im isolierten Wiederherstellungsnetzwerk und
   prüfen Sie den OEM-Kernel, beide Sensoren, das Funkmodul und das
   Netzwerkverhalten.

Befehle für bestimmte Programmiergeräte werden hier bewusst nicht
verallgemeinert: Verkabelung der Klemme, Spannung des Controllers und
unterstützte Flash-Bausteine unterscheiden sich. Halten Sie sich an die
Dokumentation des Programmiergeräts und des Flash-Herstellers. Legen Sie an
einen SPI NOR für 3,3 V niemals 5 V an.

## Nach der Wiederherstellung

Gehen Sie davon aus, dass alle Zugangsdaten und privaten Schlüssel, die
während eines Sicherheitsvorfalls vorhanden waren, kompromittiert sein können.
Ändern Sie das Administratorpasswort, ersetzen Sie die hinterlegten
SSH-Schlüssel und tauschen Sie die Wi-Fi-Zugangsdaten aus. Überprüfen Sie die
Isolation durch den Router erneut, bevor Sie die Kamera wieder regulär nutzen.

Halten Sie fest, warum die Wiederherstellung nötig war, und bewahren Sie
bereinigte Protokolle auf. Laden Sie keinesfalls rohe Flash-Images, Schlüssel,
Tokens, Cookies oder Wi-Fi-Konfigurationen in ein Issue hoch.

## Eine feste Adresse im falschen Netzwerk

Eine feste Adresse bleibt über einen Neustart hinweg erhalten, und ein neuer
Router liegt möglicherweise in einem anderen Subnetz. Das Zurücksetzen auf
Standardwerte stellt die Adresse wieder auf automatische Vergabe um und behält
das Wi-Fi-Netzwerk bei; der Weg zurück besteht also darin, die Reset-Taste für
5 Sekunden gedrückt zu halten. Ist die Taste nicht erreichbar, hilft ein
temporärer Router im alten Subnetz mit der Adresse des alten Gateways, denn
der Medien-Daemon startet erst, wenn dieses antwortet. Anschließend geht es in
der Admin-Oberfläche unter Network weiter.

## Die Wiederherstellung ist erst abgeschlossen, wenn

- das Gerät bei wiederholten Kaltstarts jedes Mal hochfährt;
- beide CV2005-Pfade das erwartete Video liefern;
- die SKW6316-Schnittstelle stabil ist;
- der vorgesehene Verwaltungsweg funktioniert;
- unerwartete Ports und ausgehender Datenverkehr nicht vorhanden oder
  blockiert sind;
- die neue vollständige Flash-Auslesung samt Hash archiviert ist.
