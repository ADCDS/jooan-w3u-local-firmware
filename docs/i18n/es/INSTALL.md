<!-- Translated from docs/INSTALL.md. Authoritative version: ../../INSTALL.md -->

# Instalación

Esta guía está pensada para quien tenga una cámara JOOAN W3-U y sepa seguir
instrucciones y copiar comandos, aunque no domine Linux. Los mantenedores que
generan sus propias versiones encontrarán lo que necesitan en
[Compilación y publicación](../../../docs/BUILDING.md).

<a id="read-this-first"></a>
## Lee esto primero

A fecha de 2026-11-09, estas son las versiones publicadas en
[GitHub Releases](https://github.com/ADCDS/jooan-w3u-local-firmware/releases):

- **v0.2.21** (2026-10-16) es la que siguen los pasos numerados de esta guía. Se
  instala por Wi-Fi, sin hardware adicional. Obtienes una consola web local, SSH,
  vídeo RTSP de las dos lentes y, para un NVR como Frigate, pan/tilt y presets
  ONVIF; además, la cámara deja de poder comunicarse con la nube de JOOAN. El vídeo
  sigue procediendo del programa de cámara del fabricante, que continúa
  ejecutándose confinado.
- **v0.2.40** (2026-11-09) es la última versión. Añade el daemon de medios abierto
  de [`media/`](../../../media/), y es la que describe el resto de este repositorio.
  Ya se ha llevado dos veces una cámara del estado de fábrica a v0.2.40 y a su capa
  de medios siguiendo estas instrucciones. Antes de elegirla, consulta
  [La capa de medios](#the-media-layer), incluido lo que se ha probado y lo que no.
- **Cada una solo se instala en una cámara cuyo firmware del fabricante coincida
  con el de la unidad verificada.** Antes de escribir nada, el instalador compara
  los archivos del fabricante con los de la unidad verificada: en v0.2.21, toda la
  partición de aplicaciones; en v0.2.40, los doce archivos de los que depende. Si
  difieren, se niega a continuar y deja la cámara tal como estaba. Además, la capa
  de medios exige más a tu partición de aplicaciones: deben estar presentes todos
  los archivos que conserva, y la cámara debe tener los sensores CV2005. Aún no se
  sabe si las cámaras que se venden hoy traen el mismo firmware del fabricante.
- Ninguna versión tiene «soporte» todavía. Todas las publicadas hasta ahora son
  versiones preliminares de ingeniería, probadas en una sola cámara.

## ¿De forma inalámbrica o con hardware?

| Qué | Cómo | Hardware adicional |
|---|---|---|
| Primera instalación | Por Wi-Fi, mediante el actualizador web integrado en el firmware del fabricante | Ninguno |
| Actualizaciones posteriores y desinstalación | Por Wi-Fi, desde la sección Firmware de la consola web | Ninguno |
| Copia de seguridad de la flash de la cámara | Por SSH, después de la instalación ([paso 7](#7-back-up-the-camera-over-ssh)) | Ninguno |
| Copia de seguridad o restauración a nivel de chip | Con un programador de flash SPI y una pinza SOIC-8, con la cámara abierta | Opcional para la instalación. Es la única forma de recuperar una cámara que ya no arranca |
| Capa de medios (v0.2.40) | Por Wi-Fi y SSH. Reescribe la partición de aplicaciones de la cámara | Ninguno para instalarla. Si se corta la corriente mientras escribe, un programador y una copia de seguridad son la única forma de recuperar la cámara |

**Para instalar v0.2.21 no necesitas tarjeta microSD, cable USB, adaptador serie
ni programador de flash, y tampoco hace falta abrir la cámara.** Estas
instrucciones no usan el cable Ethernet de la cámara.

Nunca hace falta una tarjeta microSD. Si hay una insertada, el firmware guarda en
ella los registros de fallos. En su modo de arranque de respaldo, la capa de medios
también puede ejecutar sus archivos de programa desde una tarjeta, pero eso es
terreno de los mantenedores, no una vía de instalación (consulta
[La capa de medios](#the-media-layer)).

## Qué necesitas

- **La cámara.** Debe ser una JOOAN W3-U cuya etiqueta o app del fabricante indique
  el modelo `JA-A12`. Otras cámaras que se venden como «W3-U» usan chips distintos:
  lee [Compatibilidad](../../../docs/COMPATIBILITY.md). El instalador comprueba el
  modelo y los archivos del fabricante, y rechaza las cámaras que no coinciden. No
  puede ver la placa de circuito, así que abrir la cámara es la única comprobación
  completa (Compatibilidad explica qué hay que buscar).
- **La app de JOOAN para móvil.** La app es la forma en que el firmware del
  fabricante se conecta a tu Wi-Fi. Este proyecto no tiene otra manera de conectar
  a la red una cámara de fábrica.
- **Un router Wi-Fi** en el que:
  - la cámara pueda unirse a una red WPA2. La cámara no pudo unirse a un punto de
    acceso OpenWrt con radio Qualcomm ath11k. Los puntos de acceso basados en
    Realtek funcionaron (consulta [Compatibilidad](../../../docs/COMPATIBILITY.md#wi-fi-access-point-interoperability));
  - puedas bloquear el acceso a Internet de un solo dispositivo. En los routers
    aparece como control parental, control de acceso o regla de firewall;
  - tu PC esté en la misma red que la cámara.
- **Un PC** con Windows 10 o posterior, macOS o Linux, que tenga:
  - Python 3.9 o posterior;
  - el paquete de Python `cryptography`;
  - un cliente SSH. Viene integrado en Windows 10 y posteriores, macOS y Linux.
- **Unos 30 minutos** y una fuente de alimentación estable para la cámara (5 V,
  1.5 A). No desconectes la cámara mientras se está instalando.

## Paso a paso

### 1. Configura la cámara con la app de JOOAN

Sigue el proceso de configuración normal de la app y conecta la cámara a la red
Wi-Fi en la que se va a quedar. Cambiar más adelante la red de la cámara es más
complicado (consulta el [paso 8](#8-use-the-camera)). La app empareja la cámara a
través de la nube de JOOAN, así que haz esto mientras la cámara todavía pueda
acceder a Internet; el paso 3 se lo bloquea después. (El firmware del fabricante
también ofrece un punto de acceso de configuración y una vía por DHCP a través de
Ethernet, que quizá permitirían prescindir de la app; ninguna de las dos se ha
probado con esta guía.)

Si la app ofrece una actualización del firmware de la cámara, recházala. Un
firmware del fabricante más reciente cambia los archivos que comprueba el
instalador, y entonces el instalador rechaza la cámara.

**Ha funcionado si** la app muestra vídeo en directo.

### 2. Averigua la dirección IP de la cámara

Búscala en la lista de dispositivos conectados de tu router. El firmware del
fabricante pide una dirección al router con el nombre `JAIPC`. La dirección MAC de
la cámara figura en la información del dispositivo de la app. Conviene reservar la
dirección para la cámara en los ajustes DHCP del router, para que no cambie.

**Ha funcionado si** la cámara responde a `ping CAMERA_IP` desde tu PC. Aquí y en
adelante, sustituye `CAMERA_IP` por la dirección que hayas encontrado, por ejemplo
`192.168.1.50`.

### 3. Bloquea el acceso a Internet de la cámara

En el router, bloquea el acceso a Internet de la cámara, y solo de la cámara.
Mantén tu PC en la misma red.

Durante unos segundos en cada encendido, el servidor web y el actualizador del
fabricante se ejecutan antes de que este firmware tome el control. El bloqueo
mantiene ese intervalo aislado de Internet. A partir de ahora, la app de JOOAN ya
no puede comunicarse con esta cámara. Es lo esperado.

**Ha funcionado si** la app muestra la cámara como desconectada y `ping CAMERA_IP`
sigue respondiendo.

### 4. Prepara el PC

Descarga en una misma carpeta nueva:

1. El **código fuente de v0.2.21**, desde
   <https://github.com/ADCDS/jooan-w3u-local-firmware/archive/refs/tags/v0.2.21.zip>.
   Descomprímelo. Obtendrás una carpeta `jooan-w3u-local-firmware-0.2.21`.
2. Estos seis archivos de la
   [página de la versión v0.2.21](https://github.com/ADCDS/jooan-w3u-local-firmware/releases/tag/v0.2.21).
   Guárdalos juntos en una carpeta llamada `release`, al lado de la carpeta del
   código fuente: `JOOAN_FW_PKG`, `JOOAN_FW_PKG.sha256`, `JOOAN_UNINSTALL`,
   `JOOAN_UNINSTALL.sha256`, `manifest.json` y `manifest.json.sig`.

El código fuente debe ser el de v0.2.21, no el de la rama `main` actual. La
herramienta de subida actual rechaza el paquete de v0.2.21 con el mensaje
`signed release target/ABI contract does
not match this repository`.

Instala `cryptography`.

Linux o macOS:

```sh
python3 -m pip install --user cryptography
```

En Debian o Ubuntu también sirve `sudo apt install python3-cryptography`.

Windows (Símbolo del sistema):

```bat
py -m pip install cryptography
```

**Ha funcionado si** esto muestra un número de versión:

```sh
python3 -c "import cryptography; print(cryptography.__version__)"
```

En Windows, escribe `py` en lugar de `python3`, aquí y en el paso 5.

### Entrega inalámbrica (OTA): cómo recibe realmente la cámara el paquete

Lee esto antes del paso 5. El actualizador de fábrica funciona por **descarga**
(pull) desde la nube, no por subida. `tools/upload_ota.py` lo activa, pero lo que
envía por POST se ignora: a continuación, la cámara descarga el firmware desde una
URL fija de la nube, `https://use1api.jooaniot.com/fw/JOOAN_FW_PKG`, mediante TLS.
Por eso, además de ejecutar la herramienta, una instalación tiene dos requisitos:

1. **Para la cámara, `use1api.jooaniot.com` debe resolverse a la dirección del PC
   que va a servir el paquete.** Esta es la parte que más gente pasa por alto. La
   cámara **ignora el servidor DNS que tu router reparte por DHCP** y consulta
   servidores DNS públicos fijados en el código (223.6.6.6 y 8.8.8.8). Cambiar
   «tu DNS» no sirve de nada. La redirección tiene que hacerse en la ruta de red.
   Dos formas que funcionan:

   - **Un router que redefina ese nombre para la cámara.** En OpenWrt, basta una
     sola línea de dnsmasq, `address=/use1api.jooaniot.com/<PC-IP>`, más una regla
     de firewall que **redirija el tráfico del puerto 53 de la propia cámara al
     servidor DNS del router** (un DNAT del UDP/TCP 53 de la cámara hacia el
     router), porque, si no, la cámara se salta el DNS del router. Limita ambas a
     la MAC de la cámara.
   - **Un equipo de redirección DNS en la subred de la cámara** que responda a ese
     único nombre con la dirección del PC y reenvíe todo lo demás, de nuevo con el
     puerto 53 de la cámara dirigido hacia él.

   No hace falta simular el resto de la nube: solo este nombre de host, y solo para
   la descarga del firmware.

2. **El PC debe servir el paquete en el puerto TCP 443.** `tools/serve_ota.py` hace
   exactamente eso y nada más: responde a `/fw/JOOAN_FW_PKG` con el archivo que le
   indiques, mediante un certificado autofirmado (la cámara no lo valida), y
   responde con un 404 a cualquier otra ruta. No suplanta la API de la nube, no
   reenvía nada a otro servidor y no toca las credenciales del dispositivo.

Si tu cámara ya ejecuta jooan-local (una actualización posterior, no una primera
instalación), nada de esto se aplica: las actualizaciones pasan por la API
autenticada `/api/v1/update`, no por la descarga desde la nube.

### 5. Instala

Sirve el paquete. Desde la carpeta `jooan-w3u-local-firmware-0.2.21`, en una
terminal (hacen falta privilegios para escuchar en el puerto 443; en Linux, usa
`sudo`):

```sh
sudo python3 tools/serve_ota.py --pkg ../release/JOOAN_FW_PKG
```

Muestra `[ready] https://0.0.0.0:443/fw/JOOAN_FW_PKG` y genera un certificado
desechable para `use1api.jooaniot.com`. Déjalo en ejecución. Configura la
redirección DNS de la sección anterior para que la cámara llegue a este servidor.

Después, en una segunda terminal, desde la misma carpeta, activa el actualizador de
la cámara:

```sh
python3 tools/upload_ota.py --host CAMERA_IP --pkg ../release/JOOAN_FW_PKG
```

La herramienta comprueba la firma del paquete, activa el actualizador del
fabricante de la cámara (que descarga desde el servidor que iniciaste antes) y
espera a que la cámara se reinicie con el nuevo firmware. La terminal del servidor
muestra `[served] /fw/JOOAN_FW_PKG -> ... bytes` cuando la cámara lo descarga.
Cuenta con que tarde entre uno y cinco minutos. Mantén la cámara conectada a la
corriente.

**Ha funcionado si** la última línea es:

```text
[+] jooan-local is active
```

Si falla:

| Mensaje | Qué significa y qué hacer |
|---|---|
| `error: signed release target/ABI contract does not match this repository` | Estás ejecutando otra versión del código fuente. Usa el zip de v0.2.21 del paso 4 |
| `... requires the Python 'cryptography' package` | Instálalo como en el paso 4 |
| `[-] unexpected OEM response HTTP ...` | El actualizador del fabricante de la cámara rechazó la activación. Detente aquí |
| La terminal del servidor nunca muestra `[served] ...` | La cámara no pudo llegar a tu servidor. Casi siempre la causa es la redirección DNS: confirma que `use1api.jooaniot.com` se resuelve a tu PC **desde el lado de la cámara** y que el puerto 53 de la cámara está redirigido (la cámara ignora el DNS que reparte el DHCP). Comprueba que el puerto 443 está abierto en el PC |
| `[-] package upload completed but jooan-local did not become healthy` | La cámara aceptó el paquete, pero este firmware no arrancó. Lo más probable es que el instalador encontrara archivos del fabricante distintos y rechazara la cámara antes de escribir nada. La cámara conserva su firmware del fabricante. Detente aquí y consulta [Si algo sale mal](#if-something-goes-wrong) |
| Ninguna respuesta | Comprueba `CAMERA_IP` y que el PC y la cámara estén en la misma red |

### 6. Inicia sesión

Abre `http://CAMERA_IP/` en un navegador e inicia sesión:

```text
username: admin
password: admin
```

La contraseña sigue siendo `admin` hasta que la cambies en la sección System. La
consola web, SSH y RTSP usan la misma contraseña: al cambiarla, cambia en los tres.

**Ha funcionado si** `http://CAMERA_IP/api/v1/setup/status` muestra
`"release_version":"0.2.21"`.

<a id="7-back-up-the-camera-over-ssh"></a>
### 7. Haz una copia de seguridad de la cámara por SSH

Recomendado. Así se guarda en tu PC una copia de cada partición de la flash, sin
hardware adicional. Si una actualización posterior sale mal, solo esta copia puede
recuperar esta cámara: los archivos del fabricante que contiene no están publicados
en ningún sitio. Para restaurar una cámara que ya no arranca también hace falta un
programador de flash (consulta [Recuperación](./RECOVERY.md#external-spi-recovery)).

La cámara tiene siete particiones, de `mtd0` a `mtd6`. Este comando las enumera:

```sh
ssh admin@CAMERA_IP cat /proc/mtd
```

Después, guarda cada partición. Ejecuta cada comando e introduce la contraseña
cuando se te pida. En Windows, usa el Símbolo del sistema, no PowerShell. El `>` de
Windows PowerShell daña los archivos binarios.

```sh
ssh admin@CAMERA_IP "cat /dev/mtdblock0" > camera-mtd0.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock1" > camera-mtd1.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock2" > camera-mtd2.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock3" > camera-mtd3.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock4" > camera-mtd4.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock5" > camera-mtd5.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock6" > camera-mtd6.bin
```

**Ha funcionado si** los tamaños de los siete archivos suman 8388608 bytes (8 MiB)
y, para `camera-mtd4.bin` (la partición de aplicaciones), la suma MD5 en la cámara:

```sh
ssh admin@CAMERA_IP md5sum /dev/mtdblock4
```

coincide con la del PC: `md5sum camera-mtd4.bin` (Linux),
`md5 camera-mtd4.bin` (macOS) o `certutil -hashfile camera-mtd4.bin MD5`
(Windows).

Estos archivos contienen la identidad de esta cámara, su contraseña Wi-Fi y sus
claves. Guárdalos en privado, no los compartas nunca y nunca los escribas en otra
cámara.

<a id="8-use-the-camera"></a>
### 8. Usa la cámara

Con v0.2.21:

- **Vídeo:** RTSP en el puerto 554, usuario `admin` y la contraseña de
  administrador. `rtsp://CAMERA_IP:554/live/ch0` es la lente del cabezal pan/tilt y
  `rtsp://CAMERA_IP:554/live/ch1` es la lente fija.
- **Pan/tilt y presets:** ONVIF en `CAMERA_IP`, puerto 80, con el mismo usuario y
  la misma contraseña.
- **SSH:** `ssh admin@CAMERA_IP`, con la misma contraseña.
- **Wi-Fi:** la cámara se queda en la red que configuró la app. Para cambiarla de
  red, introduce la nueva red en Network → Wi-Fi de la consola web, prepárala
  (Stage) y confírmala (Commit) cuando la cámara se haya vuelto a conectar. Un
  cambio que nunca se confirma se revierte.
- **Reloj:** la cámara no tiene un reloj alimentado por batería. Tras un corte de
  corriente, pulsa «Sync time & zone to this device» (sincronizar hora y zona con
  este dispositivo) en System → Camera time.

La documentación de v0.2.21 describe estas funciones. Léela en la etiqueta de esa
versión:
[README](https://github.com/ADCDS/jooan-w3u-local-firmware/blob/v0.2.21/README.md)
y [API](https://github.com/ADCDS/jooan-w3u-local-firmware/blob/v0.2.21/docs/API.md).
Los documentos de `main` describen la 0.2.40.

<a id="updating-later"></a>
## Actualizar más adelante

Instala una versión publicada más reciente desde la consola web. En la sección
Firmware, elige su `JOOAN_FW_PKG`, espera a que aparezca «signature verified»
(firma verificada) y aplícalo. La cámara se reinicia y queda sin conexión unos 90
segundos. Solo se acepta una versión si está firmada por este proyecto y es más
reciente que la instalada.

**No actualices una cámara con v0.2.21 a la 0.2.22 o posterior salvo que vayas a
instalar la capa de medios justo después.** A partir de la 0.2.22, la plataforma ya
no sirve vídeo por sí misma, así que hasta entonces la cámara no tiene vídeo.

Las versiones posteriores a la 0.2.21 solo responden cuando se accede a ellas
mediante una dirección IP privada o mediante el propio nombre de la cámara. El
nombre es `jooan-w3u`, salvo que lo cambies en Network → Local name (Red → Nombre
local). Puede usarse solo o terminado en `.local`, `.lan`, `.home.arpa` o
`.internal`, como en `jooan-w3u.local` o `jooan-w3u.lan`. Cualquier otro nombre
recibe `403 host_rejected` (consulta
[Seguridad de red](../../../docs/NETWORK-SECURITY.md#host-and-origin-checks)).

## Desinstalar

En la sección Firmware, aplica el archivo `JOOAN_UNINSTALL` de la versión que
instalaste. Tras el reinicio, la cámara vuelve a ejecutar su firmware del
fabricante sin confinamiento: vuelve el servidor web del fabricante, y la cámara
puede volver a comunicarse con la nube salvo que el router siga bloqueándola. La
cámara conserva los ajustes Wi-Fi propios del fabricante. Los ajustes de la
plataforma, incluida una red Wi-Fi que se haya confirmado en ella, se eliminan.
Encontrarás los detalles en [Recuperación](./RECOVERY.md#booting-camera-rollback).

**Si la capa de medios está instalada, la desinstalación no la elimina.** Sus
archivos en `/opt/jooanipc-open` se quedan, incluidas la contraseña de flujo y la
clave privada TLS, así que bórralos antes por SSH: `rm -rf /opt/jooanipc-open`. La
partición de aplicaciones también sigue siendo la reducida, sin los mensajes de voz
del fabricante ni los controladores de otros sensores. **No intentes volver a
escribir la partición original con la cámara en funcionamiento:** el programa que se
ejecuta desde esa partición falla en cuanto esta empieza a borrarse, y si la placa se
reinicia antes de que termine la escritura, solo un programador de flash podrá
recuperar la cámara. Para restaurar la partición original hacen falta un programador
de flash y el `my-appfs.img` que guardaste.

<a id="if-something-goes-wrong"></a>
## Si algo sale mal

- **La cámara arranca con su firmware del fabricante después del paso 5.** Si el
  instalador rechazó la cámara, no escribió nada y la cámara sigue igual. Puedes
  volver a usar la app de JOOAN quitando el bloqueo del router.
- **La consola web o SSH dejan de responder después de una actualización.** Apaga y
  vuelve a encender la cámara una vez y espera tres minutos. Si continúa sin
  responder, sigue las instrucciones de [Recuperación](./RECOVERY.md).
- **La cámara ya no arranca en absoluto.** Solo una restauración de tu copia de
  seguridad a nivel de chip, con un programador de flash, puede recuperarla
  ([Recuperación](./RECOVERY.md#external-spi-recovery)).

Cuando pidas ayuda, no publiques nunca imágenes de la flash, contraseñas ni claves
(consulta la [Política de seguridad](../../../SECURITY.md#reporting-a-vulnerability)).

<a id="the-media-layer"></a>
## La capa de medios

A partir de la 0.2.22, el vídeo, ONVIF y el pan/tilt los proporciona el daemon de
medios abierto [jooanipc-open](../../../media/) en lugar del programa del
fabricante. En un arranque nativo, el predeterminado desde la 0.2.23, el programa
del fabricante nunca llega a iniciarse. Esa es la configuración que describe el
resto de este repositorio.

**Toda esta cadena ya se ha ejecutado dos veces en una cámara real, desde el
firmware de fábrica hasta la pila de software abierta.** Una cámara de pruebas se
devolvió a su firmware de fábrica original, se conectó a la Wi-Fi sin la app del
fabricante, se actualizó a la plataforma de forma inalámbrica (OTA) y, después, se
le grabó una imagen de medios generada a partir de su propia partición. Sirvió vídeo
por RTSP y superó un reinicio en frío con la pila de software abierta. Qué comprobó
la primera ejecución y qué sigue estando poco probado:

| Paso | Qué se comprobó |
|---|---|
| Vuelta al firmware de fábrica | Se volvió a grabar la partición de fábrica en la cámara de pruebas (alimentando el watchdog y con reversión desde RAM), y arrancó con el firmware de fábrica original, sin aprovisionar |
| Aprovisionamiento sin app | Desde un PC cercano, un `GET /goform/setupWifiInfo` al SoftAP de la cámara la pasó a la Wi-Fi; la cámara informó de que todo había ido bien y se unió a la red |
| Instalación inalámbrica (OTA) de la plataforma | Con la nube redirigida a un servidor local, la cámara descargó y aplicó el paquete firmado de la plataforma, y arrancó con él |
| La imagen | Generada a partir de la partición de fábrica real; contiene los mismos archivos, byte a byte, que una imagen de medios que se sabe que funciona |
| La configuración | Lo que escribe el instalador supera la comprobación propia del daemon incluido en la versión |
| El instalador | Sus decisiones, su orden y sus comprobaciones previas, tanto frente a un sustituto simulado como frente a la cámara real; se negó correctamente a continuar hasta que la plataforma y la Wi-Fi estuvieron listas |
| El script de grabación, `first-appfs.sh` | Se ejecutó de verdad: grabó la imagen de medios, mantuvo la reversión desde RAM, y el daemon abierto validó el arranque nativo y mantuvo alimentado el watchdog |
| Supera el reinicio | Tras un ciclo completo de apagado y encendido, la cámara volvió con la imagen de medios abierta y sirvió RTSP, sin revertir al firmware de fábrica |
| Instalación de la 0.2.39 desde el firmware de fábrica | Hecha con el mismo actualizador durante esta ejecución |

Dos problemas menores detectados en esa ejecución se corrigieron en v0.2.40, y
después se ejecutó toda la cadena por segunda vez desde el firmware de fábrica, ya
con esa versión:

- Ahora se puede confirmar a través de la plataforma una red Wi-Fi **abierta** (sin
  contraseña), a la que se une con `key_mgmt NONE`; las redes WPA2 funcionan como
  antes.
- Con una plataforma recién instalada, el `/opt` de 384 KiB puede quedarse justo
  corto para la primera instalación de medios. Ahora el instalador libera
  automáticamente el `/opt/conf/config.org` de fábrica, que es regenerable, cuando
  falta espacio y está presente la copia de fábrica para restaurarlo (el arranque de
  fábrica vuelve a crear ese archivo a partir de `/mnt/mtd/etc`).

Si la grabación no supera la verificación por relectura, el script restaura la
partición a partir de una copia en RAM. **Si se corta la corriente mientras
escribe, solo un programador de flash y una copia de seguridad de esa partición
pueden recuperar la cámara** (consulta
[Recuperación](./RECOVERY.md#appfs-reflash-recovery)). La grabación ya se ha
completado en una cámara real varias veces a lo largo de los dos recorridos
completos, pero solo en esa única unidad; haz antes una copia de seguridad
completa de todas las particiones (el instalador la hace) y mantén la cámara con una
alimentación estable durante los pocos minutos que tarda.

Necesitas un ordenador con Linux o WSL, con `squashfs-tools`, Python 3, OpenSSH y
OpenSSL, y los archivos de v0.2.40 de la página de la versión.

1. **Instala la plataforma.** Sigue los pasos 4 y 5 con el código fuente y los
   archivos de v0.2.40 o, desde v0.2.21, actualiza como se explica en
   [Actualizar más adelante](#updating-later). A partir de ahora, la cámara no tiene
   vídeo hasta el último paso. La consola web sigue en `http://CAMERA_IP/`.
2. **Haz una copia de seguridad de la cámara** ([paso 7](#7-back-up-the-camera-over-ssh)).
3. **Confirma su red Wi-Fi** en Network → Wi-Fi de la consola web, aunque sea la
   misma en la que ya está la cámara. Un arranque nativo solo usa la red confirmada
   allí, no la que configuró la app de JOOAN.
4. **Descomprime** `jooanipc-open-*.tar.gz` y `libimp-*.tar.gz` uno junto al otro,
   y abre una terminal en la carpeta `jooanipc-open-*`.
5. **Copia la partición de aplicaciones de tu cámara y genera tu imagen a partir de
   ella.** Ambas contienen archivos del fabricante, así que guárdalas en privado:

   ```sh
   ssh admin@CAMERA_IP 'cat /dev/mtdblock4' > my-appfs.img
   sh media/tools/build-appfs.sh --first my-appfs.img bin/jooanipc-open ../libimp-*/libimp.so bin/libatomic.so.1 my-bundle
   ```

   El script rechaza una partición a la que le falte algún archivo que necesita, o
   que contenga un programa del fabricante distinto del que fija la plataforma.

6. **Comprueba la cámara.** Esto no cambia nada:

   ```sh
   CAMERA_PASSWORD=admin python3 media/scripts/install-camera.py my-bundle --camera CAMERA_IP
   ```

7. **Instala,** eligiendo la contraseña de los flujos de vídeo de la cámara:

   ```sh
   CAMERA_PASSWORD=admin STREAM_PASSWORD=your-choice python3 media/scripts/install-camera.py my-bundle --camera CAMERA_IP --yes-untested
   ```

   Mantén la cámara conectada a la corriente. **Ha funcionado si** la última línea
   indica que la versión está en ejecución. La interfaz de administración está ahora
   en `https://CAMERA_IP/`, con el certificado propio de la cámara, así que el
   navegador mostrará un aviso la primera vez. Inicia sesión con la contraseña de
   administrador. `http://CAMERA_IP/` ahora solo redirige allí.

El arranque se comprueba a sí mismo: si un arranque nativo registra un fallo, o se
interrumpe tres veces seguidas, el siguiente arranque recurre al programa del
fabricante. Lo mismo ocurre con uno que siga sin puerta de enlace al cabo de 5
minutos, hasta que un arranque nativo se haya validado una vez (consulta
[Recuperación](./RECOVERY.md#a-media-change-that-stops-native-boot)).

**En Windows,** genera el paquete (bundle) en WSL y después ejecuta el instalador
con `py` en el Símbolo del sistema, usando `set` para cada contraseña. Usa el
OpenSSH que viene con Windows y necesita `openssl` en el PATH (Git for Windows lo
incluye). OpenSSH 8.4 o posterior (Windows 11) toma la contraseña de
`CAMERA_PASSWORD`; la versión de Windows 10 la pide en cada conexión, así que añade
antes una clave SSH en la consola web. Esto tampoco se ha probado aún en Windows.

**Las actualizaciones de medios posteriores** se generan con `build-appfs.sh` sin
`--first`, a partir de la partición actual de tu cámara y de un `bundle.json` que
escribes tú para ello (consulta
[paquetes de versión de la cámara](../../../media/docs/hardware.md#camera-release-bundles)).
En ese caso, el instalador también necesita `MEDIA_PASSWORD`, la contraseña de
flujo, para verificar el resultado. El formulario Sistema → Medios (System → Media)
de la interfaz de administración solo acepta paquetes firmados con la clave de
publicación de este proyecto. Toda actualización de medios, ya sea con el instalador
o desde ese formulario, reescribe la misma partición que la primera instalación, así
que el aviso anterior sobre los cortes de corriente también se aplica a ella,
incluidos los reinicios de la propia placa. Como una imagen contiene archivos del
fabricante, no se publica ninguna, de modo que ese formulario es para quien tenga la
clave o para quien genere la plataforma con su propia clave.

**Tarjeta SD:** el modo de arranque de respaldo de la capa de medios puede ejecutar
el daemon desde una tarjeta microSD en lugar de desde la partición de aplicaciones.
Solo el programa del fabricante monta la tarjeta, así que esto solo funciona en ese
modo de respaldo. No es una forma de saltarse ninguno de los pasos anteriores.

El formato de un paquete se describe en
[paquetes de versión de la cámara](../../../media/docs/hardware.md#camera-release-bundles).
