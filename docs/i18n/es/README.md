<!-- Translated from README.md. Authoritative version: ../../../README.md -->

# Firmware local para JOOAN W3-U

<p align="center">
  <img src="../../../docs/images/w3u-camera.jpg" width="320"
       alt="La cámara JOOAN W3-U sostenida en una mano: dos lentes, una encima de otra, en una misma carcasa blanca con pan/tilt, dos antenas Wi-Fi y un cable Ethernet corto">
  <br>
  <sub>La cámara para la que está pensado este proyecto: JOOAN W3-U (<code>JA-A12</code>), de doble lente.</sub>
</p>

Firmware que prioriza el funcionamiento local, para una cámara muy concreta: la JOOAN W3-U
(`JA-A12`). Desconecta la cámara de la nube y de los servicios P2P de JOOAN y los sustituye
por servicios locales y autenticados: una consola web, SSH, vídeo RTSP y, para un NVR
como Frigate, pan/tilt y presets ONVIF.

No se trata de un firmware de código abierto completo. Se conservan el bootloader del
fabricante, el kernel Linux 3.10, los módulos del kernel, el ajuste de los sensores y el
firmware de la radio, y también el programa de cámara del fabricante, `jooanipc`, como
respaldo. Encima se instalan dos capas:

- la **plataforma** (la raíz de este repositorio) se instala en la partición `/opt`, de
  lectura y escritura. Proporciona la consola web y la API, SSH, Wi-Fi, reloj y
  actualizaciones firmadas, y mantiene la cámara fuera de Internet;
- el **daemon de medios** [jooanipc-open](../../../media/) se instala en la partición de
  aplicaciones. Sirve el vídeo, ONVIF y el pan/tilt, de modo que en un arranque normal
  (nativo) el programa del fabricante nunca llega a iniciarse.

[Arquitectura](../../../docs/ARCHITECTURE.md) explica los modos de arranque y el límite de
confianza.

<p align="center">
  <img src="../../../docs/images/webui-overview.png" width="480"
       alt="La página Resumen de la consola web local: las vistas de las dos lentes (aquí difuminadas) y acciones rápidas de visión nocturna, luz blanca, modo privacidad, conversación y sirena">
  <br>
  <sub>La consola web local (con las vistas de las dos lentes difuminadas). Se ejecuta en la cámara,
  no necesita la nube y está disponible en siete idiomas. Más vistas a continuación.</sub>
</p>

## Estado

- **Versión preliminar de ingeniería; aún no hay ninguna versión con soporte.** Las versiones se
  prueban en una sola cámara física. Una versión solo pasa a tener soporte cuando supera en
  esa cámara las comprobaciones de instalación y desinstalación firmadas, migración, Wi-Fi,
  hora de red, SSH, enrutamiento, corte de corriente y recuperación en frío
  ([criterios de promoción en hardware](../../../tests/TEST_PLAN.md#hardware-promotion-gates)).
- **Última versión: v0.2.40** (2026-11-09): el paquete de la plataforma y los binarios y
  scripts de la capa de medios. Cada propietario sigue generando la imagen de su cámara a
  partir de la partición de aplicaciones de esa misma cámara, porque contiene archivos del
  fabricante; por eso no se publica ninguna imagen. Sustituye a v0.2.39 e incorpora dos
  correcciones de problemas detectados en el hardware: ahora se puede confirmar una red
  Wi-Fi abierta (sin contraseña) a través de la plataforma, y una plataforma recién
  instalada libera en `/opt` justo el espacio necesario para la primera instalación de
  medios.
- **La cadena completa ya se ha ejecutado en una cámara real, desde el firmware de fábrica
  hasta la pila de software abierta.** Una cámara de pruebas se devolvió al firmware de
  fábrica, se conectó a la Wi-Fi sin la app del fabricante, se actualizó a la plataforma de
  forma inalámbrica (OTA), se le grabó una imagen de medios generada a partir de su propia
  partición, sirvió vídeo por RTSP y funcionó con un NVR local; desde entonces se ha
  reinstalado una segunda vez partiendo del firmware de fábrica, ya con esta versión (consulta
  [el informe de la prueba de ida y vuelta](../../../docs/reports/hardware-roundtrip-20261108.md) y
  [La capa de medios](./INSTALL.md#the-media-layer)). Sigue siendo una versión preliminar:
  guarda la copia de seguridad completa que hace el instalador y mantén la cámara alimentada
  mientras se graba la memoria flash.
- **v0.2.21** (2026-10-16, solo plataforma) sigue publicada. Con ella, el vídeo sigue
  procediendo del programa del fabricante, que se ejecuta confinado. Es preferible usar
  v0.2.40.

## Alcance exacto del hardware

Usa este proyecto solo con una unidad que cumpla todo lo siguiente:

- producto/modelo que indica el firmware OEM: `JA-A12` / token de paquete `A12`;
- PCB: `JA-6621 V1.0`;
- SoC: Ingenic T23N;
- sensores: dos, `cv2005` y `cv2005s1`;
- radio: SeekWave SKW6316 por USB (`SV6160LITE` en la unidad verificada);
- memoria flash: SPI NOR de 8 MiB (unidad verificada: Puya P25Q64HA).

La caja de venta de la unidad verificada indica: marca `JOOAN`, modelo `W3-U`, FCC ID
`2BBQ4-W3U`, 5 V / 1.5 A. Eso solo identifica el nombre del producto, que por sí solo no
basta: la lista de comprobación anterior es la que decide la compatibilidad. La imagen de
Thingino llamada «Jooan W3-U» corresponde a otra revisión, con SC2336P/ATBM6132U, y no es
intercambiable. Ni el nombre del producto ni la forma de la carcasa son pruebas de
compatibilidad. Lee [Compatibilidad](../../../docs/COMPATIBILITY.md) antes de instalar
nada.

## Qué funciona y qué no

Con la 0.2.40 y su capa de medios:

- **Funciona:** RTSP para las dos lentes (1920×1080 y 1024×576 a 15 fps, H.264);
  pan/tilt ONVIF y 16 presets con nombre para Frigate; calibración del pan/tilt (homing) en
  cada arranque; volteo de imagen, modo noche/IR-cut manual y luz blanca; una única interfaz
  de administración en `https://<camera>/` para móviles y ordenadores (`http://<camera>/`
  redirige allí); SSH; Wi-Fi con lista de redes, dirección estática, preparación,
  confirmación y reversión de cambios; reinicio, reinicio diario y restablecimiento de los valores
  predeterminados; modo privacidad; patrulla entre presets; movimientos ONVIF precisos;
  hora de red opcional desde un servidor local; mDNS; actualizaciones firmadas.
- **Activado salvo con `audio=0`:** audio (micrófono en RTSP; hablar por el altavoz desde
  Frigate, la interfaz de administración o la API).
- **Falta:** detección de movimiento y de personas; zoom digital; un logotipo en la imagen;
  grabación en la cámara (desactivada en el perfil validado: graba con el NVR) y gestión de la
  tarjeta SD; cualquier acceso desde la nube o desde una app móvil.

[Comparativa con el OEM](../../../docs/OEM-COMPARISON.md) contrasta este firmware con el
del fabricante, punto por punto.

<p align="center">
  <img src="../../../docs/images/webui-system.png" width="480"
       alt="La página Sistema de la consola web, con Plataforma 0.2.39 y Medios 0.1.0 instalados, además de los controles de actualización, hora, contraseña, claves SSH y reinicio">
  <br>
  <sub>La página Sistema: versiones de plataforma y medios, actualizaciones, hora, contraseñas,
  claves SSH y restablecimiento. Todo en la cámara, sin nube.</sub>
</p>

## Instalación

Sigue la guía de [Instalación](./INSTALL.md). La primera instalación se hace por Wi-Fi
mediante el propio actualizador del firmware del fabricante, y las actualizaciones posteriores,
desde la consola web. No hace falta tarjeta SD, cable ni programador de flash, y no hay que
abrir la cámara. Un programador de flash solo es necesario para recuperar una cámara que ya no
arranca ([Recuperación](./RECOVERY.md)).

Para una instalación guiada sin terminal, el [instalador](../../../installer/) reúne toda la
secuencia —aprovisionamiento, actualización de la plataforma, copia de seguridad, generación de
la imagen y grabación— en una sola ventana. Arranca en modo de simulación (dry run), que lo
planifica todo sin tocar nada, muestra un registro en directo y pide confirmación una sola
vez, antes del único paso irreversible.

<p align="center">
  <img src="../../../docs/images/gui-running.png" width="480"
       alt="La ventana del instalador: un formulario para la dirección de la cámara, los paquetes y las claves, una casilla «Dry run» y un registro en directo con códigos de colores de los cinco pasos de la instalación">
  <br>
  <sub>El instalador de ventana única (<code>python3 installer/gui.py</code>) ejecutando los cinco pasos.</sub>
</p>

## Seguridad en resumen

- La cámara debe estar en una red que le impida el acceso a Internet. Durante unos
  segundos en cada arranque, el servidor web del fabricante se ejecuta antes de que este
  firmware tome el control. El firmware elimina las rutas predeterminadas de la cámara, pero
  no tiene filtro de paquetes, así que el aislamiento en el router es el verdadero límite.
- La consola web y la API usan HTTP sin cifrar en el puerto 80. Una petición debe dirigirse
  a la cámara por una dirección IP privada o de loopback, o por su propio nombre (por defecto
  `jooan-w3u`, solo o terminado en `.local`, `.lan`, `.home.arpa` o
  `.internal`). Cualquier otra petición recibe `403 host_rejected`.
- Para iniciar sesión se usa `admin` con la contraseña inicial `admin`, que sigue siendo
  válida hasta que se cambia en la página Sistema; SSH usa la misma contraseña. El daemon de
  medios tiene sus propias credenciales, su propio certificado TLS y un límite de intentos
  fallidos de inicio de sesión.
- `telnetd` y el servidor web del fabricante se detienen a la fuerza cada segundo. Cuando se
  ejecuta el programa del fabricante (solo en los modos de respaldo), un guardián bloquea su
  tráfico hacia la nube y mantiene sus puertos de escucha en loopback.

Detalles: [Seguridad de red](../../../docs/NETWORK-SECURITY.md) y
[Política de seguridad](../../../SECURITY.md), que también explica cómo informar de una
vulnerabilidad.

## Estructura del repositorio

| Ruta | Contenido | Licencia |
| --- | --- | --- |
| `build.sh`, `build-uninstall.sh`, `release.sh` | puntos de entrada para generar versiones ([Compilación](../../../docs/BUILDING.md)) | GPL-2.0-only |
| `packaging/` | manifiesto del dispositivo de destino exacto y ensamblado del paquete IronMan | GPL-2.0-only |
| `runtime/` | gancho de arranque, supervisor y scripts de ranuras y de mantenimiento | GPL-2.0-only |
| `src/` | `joan-daemon` (consola web y API), el guardián de contención, verificadores de versiones, herramientas de empaquetado | GPL-2.0-only |
| `web/` | la consola web | GPL-2.0-only |
| `tools/` | `upload_ota.py` (primera instalación), `make-github-release.py`, descarga del toolchain, diagnóstico de Wi-Fi | GPL-2.0-only |
| `tests/`, `ci/` | validación en el host | GPL-2.0-only |
| `docs/` | documentación | GPL-2.0-only |
| [`media/`](../../../media/) | daemon de medios `jooanipc-open`, sus scripts de arranque, herramientas de appfs e instalador de paquetes de versión | MIT |

La biblioteca `libimp.so` que carga el daemon de medios se compila a partir del fork de OpenIMP,
rama [`jooan/oem-t23-abi`](https://github.com/ADCDS/openimp/tree/jooan/oem-t23-abi),
que es un repositorio independiente.

Ni el repositorio ni sus versiones contienen secretos de fábrica, credenciales Wi-Fi,
claves de dispositivo, volcados de la flash, firmware OEM ni otros binarios del fabricante.
Cuando hace falta un componente OEM conservado, procede de la propia cámara del propietario.
Un paquete de la capa de medios incluye una imagen appfs reconstruida a partir de esa cámara,
por lo que permanece privado.

## Documentación

- [Instalación](./INSTALL.md): cómo instalar una versión publicada.
- [Compatibilidad](../../../docs/COMPATIBILITY.md): cómo saber si tu cámara es el modelo
  compatible.
- [Comparativa con el OEM](../../../docs/OEM-COMPARISON.md): lo que le falta a este firmware
  frente al del fabricante.
- [Recuperación](./RECOVERY.md): desinstalación, reversión y recuperación por SPI.
- [Seguridad de red](../../../docs/NETWORK-SECURITY.md): puertos de escucha, comprobaciones de
  la cabecera Host, Wi-Fi, aislamiento.
- [Arquitectura](../../../docs/ARCHITECTURE.md): modos de arranque, componentes conservados,
  cobertura de funciones.
- [API](../../../docs/API.md): la `/api/v1/` de la plataforma.
- [Compilación y publicación](../../../docs/BUILDING.md): toolchain, firma, presupuesto de
  tamaño, paquetes de versión, lista de comprobación de validación.
- [Daemon de medios](../../../media/README.md) e
  [integración de hardware](../../../media/docs/hardware.md).
- [Plan de pruebas](../../../tests/TEST_PLAN.md), [Componentes de terceros](../../../docs/THIRD_PARTY.md),
  [Política de seguridad](../../../SECURITY.md).

## Licencia

La plataforma (todo lo que queda fuera de `media/`) se distribuye bajo la licencia
[GPL-2.0-only](../../../LICENSE); `media/` tiene licencia MIT ([media/LICENSE](../../../media/LICENSE)).
Cada archivo lleva su propia cabecera SPDX y conserva su propia licencia, incluso
cuando un script MIT carga otro GPL en tiempo de ejecución. La plataforma enlaza las
propias bibliotecas Mbed TLS de la cámara en virtud de un permiso adicional, cuyo texto
exacto figura en [Componentes de terceros](../../../docs/THIRD_PARTY.md#mbed-tls-linking-exception).
No se concede ninguna licencia sobre artefactos OEM o de terceros que no estén en el
repositorio.
