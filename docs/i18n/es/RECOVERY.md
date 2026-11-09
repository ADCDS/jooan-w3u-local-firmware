<!-- Translated from docs/RECOVERY.md. Authoritative version: ../../RECOVERY.md -->

# Recuperación

## Niveles de recuperación

Hacen falta dos vías independientes:

- **Reversión o desinstalación del paquete**, para una cámara que arranca y en la que
  todavía funciona la gestión local o la vía de actualización del OEM.
- **Recuperación externa por SPI**, para una cámara que ya no arranca o que es inaccesible.

La regrabación de appfs que hace la capa de medios añade una tercera vía, manual, para el
único caso en el que esta deja la cámara funcionando sobre una partición dañada; consulta
[Recuperación tras la regrabación de appfs](#appfs-reflash-recovery).

Un paquete de desinstalación guardado solo en la cámara no es un método de recuperación
independiente. Guárdalo fuera del dispositivo. La restauración externa por SPI sigue siendo
la vía más sólida para una cámara que no puede arrancar ni aceptar una actualización.

## Material de recuperación

Conserva todo esto sin conexión y etiquétalo como perteneciente a una única cámara física:

- dos lecturas completas de la flash de 8 MiB, idénticas byte a byte, con sus hashes
  SHA-256, hechas por SSH una vez que la plataforma esté en funcionamiento
  ([Instalación, paso 7](./INSTALL.md#7-back-up-the-camera-over-ssh)) o con
  un programador SPI;
- fotografías de la PCB `JA-6621 V1.0`, de la orientación del chip de flash, de la
  orientación de la pinza y del cableado;
- el modelo del programador, el ajuste de tensión, el chip de flash seleccionado y un
  comando de lectura que se sepa que funciona;
- los paquetes exactos de instalación y desinstalación, sus manifiestos y sus hashes;
- una lectura de la partición appfs (`/dev/mtd4`) hecha antes de cada actualización de la
  capa de medios; el directorio `backups/` del paquete (bundle) solo contiene archivos de
  configuración;
- una copia del código fuente de la versión y de los metadatos del toolchain;
- notas, depuradas de datos sensibles, que describan el último arranque correcto y el
  estado de la red.

No compartas nunca el volcado de una cámara como imagen de recuperación para otra unidad.

<a id="booting-camera-rollback"></a>
## Reversión en una cámara que arranca

1. Pasa la cámara y el equipo de trabajo a la red de recuperación aislada.
2. Bloquea en el router todo acceso a Internet de la cámara.
3. Verifica el hash del paquete de desinstalación y los metadatos de destino `A12`.
4. Usa la vía de actualización local autenticada o el método de entrega sin conexión documentado.
5. Mantén una alimentación estable hasta que el paquete indique que ha terminado y concluya el
   reinicio.
6. Haz un arranque en frío, revisa el estado y los puertos de escucha, y verifica que se ha
   restaurado el estado previsto.

La desinstalación conserva los datos de fábrica y la configuración Wi-Fi guardada por el OEM,
pero borra el estado de la plataforma, incluida la red Wi-Fi confirmada
(`wifi.json`); después, la aplicación OEM usa su propia red guardada, que puede no ser
la que estaba usando la cámara. Si la cámara no puede autenticarse, no puede validar el
paquete o se reinicia una y otra vez, deja de intentar actualizarla por la red y recurre a
la recuperación externa.

La sustitución del runtime es una transacción de mantenimiento: el controlador firmado y el
paquete de recuperación por SSH siguen disponibles mientras se sustituye el único runtime
persistente. Durante la prueba de estado de dos minutos, el supervisor aplica la red Wi-Fi
confirmada (un arranque nativo que no tiene motivos para seguir esperando renuncia a su puerta
de enlace al cabo de 300 s) y vuelve a iniciar un candidato cuyo inicio haya fallado: `start.sh`
está limitado a 15 s, mientras que `entropy-ready.sh` espera hasta 30 s, y al principio de un
arranque nativo la entropía se acumula despacio. Si el candidato sigue fallando, o si se
interrumpe la prueba, se queda seleccionado como único runtime, porque la actualización ya borró
el anterior, y el supervisor sigue reiniciándolo; recupera la cámara mediante el SSH que
gestiona el controlador y vuelve a intentarlo o desinstala.
(Hasta que se corrigió esto, una prueba fallida eliminaba el candidato y dejaba la cámara sin
runtime; el párrafo siguiente describe ese estado.) El runtime anterior puede seguir
ejecutándose desde tmpfs hasta el reinicio de la actualización, pero no es una ranura de
reversión en la flash. Si una actualización de la plataforma falla, no se recurre al
actualizador OEM: GoAhead se sigue deteniendo a la fuerza mientras el controlador esté en
ejecución.

Sin runtime, nada aplica la red Wi-Fi confirmada. Entonces, un arranque nativo no consigue
llegar a su puerta de enlace, ve que nada lo está intentando, falla al cabo de 300 s y deja
fijada la marca `native-failed`, y los arranques posteriores ejecutan la aplicación OEM
vigilada por el guardián, que se une a su propia red guardada. En la unidad verificada, se
trata de la red de banco de pruebas del fabricante, `JOOANTEST` (abierta y sondeada como SSID
oculto). Para llegar a la cámara en ese estado:

1. Configura una red `JOOANTEST` temporal, oculta y abierta, en un punto de acceso al alcance
   de la cámara, restringida a la MAC de la cámara y en una subred aislada. La cámara se
   conecta en cuestión de segundos. Puede que no obtenga una dirección IPv4, pero su dirección
   IPv6 de enlace local responde por SSH (dropbear escucha en `[::]:22`), por ejemplo mediante
   `ssh -L 2222:[fe80::…%<bridge>]:22` en ese punto de acceso.
2. Instala el runtime como ranura estable: copia `runtime.tar.gz` y
   `runtime.md5` del directorio de preparación (stage) de la versión a `/opt/custom/jooan-local/slots/A/`
   y, después, con `common.sh` cargado en la shell, ejecuta `jl_lock; jl_write_selection A - 0; jl_unlock`.
3. Borra `/opt/jooanipc-open/native-failed`, ejecuta `sync` y reinicia. El supervisor inicia
   el runtime (y lo vuelve a iniciar si falla su primer inicio), el gancho del runtime aplica la
   red confirmada y el arranque nativo queda validado.
4. Elimina la red temporal.

## Una actualización detenida

En una cámara con poco espacio, una actualización de la plataforma borra el único runtime que
hay en la flash antes de escribir el nuevo. Si se detiene entre ambos pasos, la cámara sigue
funcionando desde la RAM, pero sin nada en la flash que pueda iniciarse la próxima vez: Sistema
(System) indica que la actualización se ha detenido, y cualquier reinicio se rechaza con `no-runtime`.

**No reinicies ni desenchufes la cámara todavía.** El siguiente arranque no tendría red y te
encontrarías en la situación de los pasos anteriores. Por SSH:

```sh
cat /opt/custom/jooan-local/state/selection
ls /opt/custom/jooan-local/slots/
```

Si la selección indica una ranura (`A - 0`), esa ranura existe y `df -k /opt` muestra al menos
51 KiB libres, no se ha perdido nada. Con menos de 50 KiB libres en el arranque, el `startapp`
del fabricante borra `/opt/etc` y `/opt/jooanipc-open`, lo que es peor que no tener runtime, así
que libera espacio primero. Lo que se puede borrar: cualquier cosa llamada `*.new`, `*.next` o
`*.previous` dentro de `/opt/custom/jooan-local` y `/opt/jooanipc-open`, y
`state/net-loss.log`. Lo que no: `config/`, `state/selection`, `controller.tar.gz`,
`recovery/`, una ranura seleccionada y, en `/opt/jooanipc-open`, `camera.ini`,
`deployment.env`, los dos scripts de arranque y las claves. Si la selección muestra `- - 0`,
repón un runtime. Antes, detén cualquier instalador que siga esperando a esta cámara:
continuaría en cuanto detectara un reinicio.

1. Localiza el paquete que falló, que sigue en la RAM hasta que se reinicie la cámara:
   ```sh
   ls /run/jooan-local/firmware.*.sqfs
   mkdir /tmp/fw && mount -t squashfs -o ro,loop /run/jooan-local/firmware.<id>.sqfs /tmp/fw
   ```
   Si ya no está, esos mismos dos archivos se encuentran en el `JOOAN_FW_PKG` de la versión. En
   tu ordenador, ejecuta
   `dd if=JOOAN_FW_PKG bs=96 skip=1 of=p.sqfs && unsquashfs p.sqfs runtime.tar.gz runtime.md5`;
   después, ejecuta `mkdir -p /tmp/fw` en la cámara y cópialos allí con
   `ssh admin@CAMERA 'cat > /tmp/fw/runtime.tar.gz' < squashfs-root/runtime.tar.gz`.
2. Cópialo en una ranura y deja que decida la suma de comprobación. Antes, comprueba que
   `df -k /opt` muestra como mínimo el tamaño de `runtime.tar.gz` más 60 KiB; si no, libera
   espacio, por el motivo explicado arriba.
   ```sh
   cd /opt/custom/jooan-local/slots
   rm -rf A.new
   mkdir A.new && cp /tmp/fw/runtime.tar.gz /tmp/fw/runtime.md5 A.new/
   [ "$(md5sum A.new/runtime.tar.gz | cut -c1-32)" = "$(cat A.new/runtime.md5)" ] && sync && mv A.new A && sync
   ```
3. Selecciónala con las funciones del propio controlador en ejecución:
   ```sh
   . /run/jooan-local/controller/boot/common.sh; jl_lock; jl_write_selection A - 0; jl_unlock
   ```

El supervisor solo lee la selección durante el arranque, así que esto no altera nada. Con
`df -k /opt` en 51 KiB o más, la cámara ya puede reiniciarse sin riesgo y se puede volver a
intentar la actualización. La página de administración mantiene bloqueados Reiniciar y
Restablecer (Restart y Reset) hasta que la cámara se haya reiniciado, así que reiníciala desde
la misma sesión SSH con `jl_reboot_asked`, que primero guarda la hora e indica al siguiente
arranque que el reinicio fue solicitado. Esto se hizo de verdad en camera1 el 2026-11-07, desde
la RAM.

<a id="a-media-change-that-stops-native-boot"></a>
## Un cambio en la capa de medios que impide el arranque nativo

Si el daemon de medios termina durante un arranque nativo, su watchdog armado reinicia la placa.
Si un arranque registró el fallo, el siguiente deja fijada la marca `native-failed` y pasa al
traspaso (handoff); un arranque que solo se vio interrumpido, por un corte de corriente o un
bloqueo, dispone antes de dos intentos más. Si el traspaso también falla, se fija la marca
`boot-trial` y la aplicación OEM vigilada sigue en ejecución. Un reinicio solicitado por la
plataforma no cuenta en absoluto como fallo del arranque que interrumpió, aunque no anula un
fallo registrado antes de él.

Un arranque nativo sin puerta de enlace espera, sin reiniciar la placa y sin dejar ninguna
marca, mientras la espera pueda dar resultado: es decir, si hay una red confirmada, un runtime
que intenta unirse a ella y un arranque anterior que ya se ha validado en esta cámara. En caso
contrario, falla al cabo de 300 s, como se ha descrito antes. Una versión que pierde la puerta
de enlace después de iniciarse queda validada igualmente, salvo que eso ocurra en tres arranques
seguidos. Si una red confirmada desaparece para siempre en una cámara que ya se ha validado
antes, la cámara se queda esperando: para llegar a ella, vuelve a poner en marcha una red con
ese nombre y esa contraseña. La plataforma y SSH siguen respondiendo, pero la interfaz HTTPS
no. Para recuperarla:

1. Averigua por qué falló en `/tmp/jooanipc-open.log` o, si la placa se ha reiniciado desde
   entonces, en un registro replicado fuera de la cámara. Busca con grep `startup failed` y
   `health check`, y en los mensajes del kernel, `Process `: un oops significa que la causa
   puede no estar en la versión.
2. Deshaz la causa, si se trata de un ajuste guardado. Por ejemplo, en
   `/opt/jooanipc-open/state/controls.json` o en `/opt/jooanipc-open/camera.ini`.
3. Ejecuta `cd /opt/jooanipc-open && rm -f native-failed native-trial native-retry boot-trial && sync`
   y reinicia. El arranque vuelve a ser nativo y el instalador acepta la cámara.

Esto le ocurrió dos veces a la cámara de pruebas el 2026-11-06, con una versión que no podía
iniciarse con el modo privacidad activado. Se recuperó de esta forma en unos 5 minutos cada vez.

Ocurrió una tercera vez ese mismo día, con una versión sin defectos: `IMP_ISP_Open failed (-1)`.
Un script de monitorización leía `/proc/jz/isp/isp-m0` por SSH mientras la placa arrancaba. Si
se lee después de que se haya cargado el módulo del ISP y antes de que el daemon haya abierto el
ISP, ese archivo provoca un oops en el driver (`Process awk` en los mensajes del kernel), y la
apertura del ISP que intentó el daemon un segundo después falló. Un arranque nativo permanece en
ese estado durante 30 s o más mientras espera a la Wi-Fi. Cualquier cosa que lea periódicamente
ese archivo debe ver antes `"ready":true` en `/api/v1/status`.

La desinstalación firmada es una eliminación destructiva, no una restauración del estado
anterior. Elimina la consola web, la poda de rutas, el guardián de procesos, el SSH del proyecto
y el controlador y el runtime comprimidos; el siguiente arranque vuelve al comportamiento
OEM/GoAhead, con la aplicación OEM ejecutándose sin confinamiento. Los archivos de la capa de
medios en `/opt/jooanipc-open` y su versión en la partición appfs permanecen en la flash, pero
nada los inicia una vez que `local.rc` ha desaparecido. La desinstalación no restaura el antiguo
gancho telnet sin autenticación ni un `/opt/etc/local.rc` anterior que se hubiera archivado.
Mantén activo el aislamiento en el router antes, durante y después de la desinstalación.

<a id="appfs-reflash-recovery"></a>
## Recuperación tras la regrabación de appfs

`media/scripts/upgrade-appfs.sh` regraba `/dev/mtd4`, la partición appfs, que contiene el
`startapp` del OEM, `jooanipc`, los módulos del kernel, el ajuste IQ de los sensores y la
versión de medios. Antes de detener el daemon de medios, toma el bloqueo de mantenimiento de la
plataforma (`/run/jooan-local/state.lock`), que pone en pausa los reinicios del runtime por parte
del supervisor y el reinicio por el watchdog de red, y un alimentador `watchdog` del rootfs se
hace cargo del watchdog de hardware armado. Sistema → Medios (System → Media) de la interfaz de
administración ejecuta este mismo script. `media/scripts/first-appfs.sh`, para la primera
versión de medios de una cámara, sigue los mismos pasos a partir de la aplicación de fábrica. Su
reversión también elimina la configuración de medios, de modo que la cámara vuelve al estado de
fábrica, solo con la plataforma.

- **Antes del punto de confirmación** (el cambio de configuración justo antes de
  `flashcp`), cualquier fallo reinicia la cámara con la partición appfs intacta.
- **Después de él**, una grabación que no supera la verificación se revierte a partir de una
  copia en RAM de la partición appfs original, con hasta tres intentos; después se restauran los
  archivos de configuración y `native-boot.sh`, y la cámara se reinicia.
- **Si la reversión también falla**, el instalador no reinicia. La cámara sigue en marcha con
  el alimentador del rootfs en ejecución, el bloqueo tomado y las dos imágenes en
  `/tmp`: `/tmp/jipc-appfs-rollback.bin` (la partición appfs original) y
  `/tmp/jipc-flash-candidate.squashfs` (la imagen nueva). SSH sigue funcionando, porque la
  plataforma se ejecuta desde `/run` y el rootfs.

Recupera ese estado por SSH, sin reiniciar:

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

Una primera instalación no guarda archivos `.before`, porque no había capa de medios. Después
del `flashcp` y de la comprobación, en lugar de las tres líneas `cp`, ejecuta:

```sh
cd /opt/jooanipc-open && rm -f boot-handoff.sh camera.ini deployment.env enabled native
```

El MD5 de la partición appfs original es el segundo argumento que recibió el instalador: el
`expected_current_appfs_md5` del paquete, que `install-camera.py` también muestra en las
comprobaciones previas. Repite `flashcp` hasta que `md5sum` coincida; no reinicies la cámara
antes de que coincida. `/tmp` está en la RAM: las imágenes se pierden con cualquier reinicio.

Si se corta la corriente, o la cámara se reinicia, mientras la partición appfs está dañada,
falta `startapp`: nada carga `local.rc`, así que no se inician ni la plataforma ni SSH. En ese
caso, la recuperación requiere un programador de flash SPI (SPI NOR de 8 MiB) y una copia de
seguridad de la partición appfs o de toda la flash, como se explica a continuación.

<a id="external-spi-recovery"></a>
## Recuperación externa por SPI

La placa verificada usa una memoria SPI NOR de 8 MiB (Puya P25Q64HA en la unidad examinada).
Comprueba qué chip lleva montado la tuya en lugar de darlo por supuesto.

1. Desconecta la alimentación y el cable Ethernet de la cámara.
2. Usa un programador que trabaje de forma segura a 3.3 V y verifica el pin 1 y la orientación
   a partir de la placa y de la hoja de datos de la flash.
3. Antes de escribir, vuelve a leer el chip y conserva esa imagen del estado fallido.
4. Confirma que la copia de seguridad mide exactamente 8 MiB y que su SHA-256 coincide con el
   de una de las lecturas previas a la instalación.
5. Escribe la imagen completa con el comando verificado del programador.
6. Vuelve a leer el chip y compara cada byte con la copia de seguridad.
7. Desconecta el programador antes de conectar la alimentación normal de la cámara.
8. Arranca solo en la red de recuperación aislada y verifica el kernel OEM, los dos sensores,
   la radio y el comportamiento de la red.

Aquí no se dan, a propósito, comandos que valgan para cualquier programador: el cableado de la
pinza, la tensión del controlador del programador y la compatibilidad con chips de flash varían.
Sigue la documentación del programador y del fabricante de la flash. No apliques nunca 5 V a
una SPI NOR de 3.3 V.

## Después de la recuperación

Da por hecho que cualquier credencial o clave privada presente durante un fallo de seguridad
puede estar comprometida. Cambia la contraseña de administrador, sustituye las claves SSH
registradas y renueva las credenciales Wi-Fi. Vuelve a verificar el aislamiento en el router
antes de devolver la cámara a su uso normal.

Anota por qué fue necesaria la recuperación y conserva los registros, depurados de datos
sensibles. No subas a una incidencia (issue) imágenes en bruto de la flash, claves, tokens,
cookies ni la configuración Wi-Fi.

## Una dirección fija en la red equivocada

Una dirección fija se mantiene tras un reinicio, y un router nuevo puede estar en otra subred.
Restablecer los valores predeterminados (Reset to defaults) vuelve a poner la dirección en
automática y conserva la red Wi-Fi, así que mantener pulsado el botón de restablecimiento
(reset) durante 5 segundos es la forma de recuperar el acceso a la cámara. Si no puedes llegar
al botón, usa un router temporal en la
subred antigua, con la dirección de la antigua puerta de enlace, porque el daemon de medios solo
se inicia cuando esta responde. Después, usa la interfaz de administración, en Red (Network).

## La recuperación solo está completa cuando

- el dispositivo arranca en frío correctamente una y otra vez;
- los dos canales CV2005 producen el vídeo esperado;
- la interfaz SKW6316 es estable;
- funciona la vía de gestión prevista;
- no hay puertos ni tráfico saliente inesperados, o bien están bloqueados;
- la nueva lectura completa de la flash está archivada junto con su hash.
