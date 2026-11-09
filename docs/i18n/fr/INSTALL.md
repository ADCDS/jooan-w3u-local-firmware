<!-- Translated from docs/INSTALL.md. Authoritative version: ../../INSTALL.md -->

# Installation

Ce guide s’adresse à toute personne qui possède une caméra JOOAN W3-U et sait
suivre des instructions et copier des commandes, sans pour autant maîtriser
Linux. Les mainteneurs qui produisent leurs propres versions consulteront
plutôt [Compilation et publication](../../../docs/BUILDING.md).

<a id="read-this-first"></a>
## À lire avant tout

À la date du 2026-11-07, deux versions sont disponibles sur
[GitHub Releases](https://github.com/ADCDS/jooan-w3u-local-firmware/releases) :

- **v0.2.21** (2026-10-16) est celle que suit ce guide, et la dernière version
  à avoir été installée sur une caméra sous firmware d’origine. Elle s’installe
  par Wi-Fi, sans matériel supplémentaire. Vous obtenez une console web locale,
  SSH, la vidéo RTSP des deux objectifs ainsi que le pilotage
  panoramique/inclinaison et les préréglages ONVIF pour un NVR tel que Frigate,
  et la caméra ne peut plus joindre le cloud JOOAN. La vidéo provient toujours
  du programme caméra du fabricant, qui continue de tourner en mode confiné.
- **v0.2.39** (2026-11-07) ajoute le démon média ouvert de
  [`media/`](../../../media/), et c’est elle que décrit le reste de ce dépôt.
  **Aucune caméra n’a encore été amenée de son firmware d’origine à la v0.2.39
  en suivant ces instructions.** Avant de la choisir, lisez
  [La couche média](#the-media-layer), y compris ce qui a été testé et ce qui
  ne l’a pas été.
- **Chacune ne s’installe que sur une caméra dont le firmware du fabricant est
  identique à celui de l’appareil vérifié.** Avant d’écrire quoi que ce soit,
  l’installateur compare les fichiers du fabricant à ceux de l’appareil
  vérifié : toute la partition applicative pour la v0.2.21, et les douze
  fichiers dont elle dépend pour la v0.2.39. S’ils diffèrent, il refuse de
  poursuivre et laisse la caméra en l’état. La couche média exige ensuite
  davantage de votre partition applicative : chaque fichier qu’elle conserve
  doit y être présent, et la caméra doit être équipée des capteurs CV2005.
  Personne ne sait encore si les caméras vendues aujourd’hui sont livrées avec
  le même firmware du fabricant.
- Aucune version n’est encore « prise en charge ». Toutes les versions publiées
  jusqu’ici sont des préversions techniques, testées sur une seule caméra.

## Sans fil ou avec du matériel ?

| Quoi | Comment | Matériel supplémentaire |
|---|---|---|
| Première installation | Par Wi-Fi, via l’outil de mise à jour web intégré au firmware du fabricant | Aucun |
| Mises à jour ultérieures et désinstallation | Par Wi-Fi, depuis la section Firmware de la console web | Aucun |
| Sauvegarde de la mémoire flash de la caméra | Via SSH, après l’installation ([étape 7](#7-back-up-the-camera-over-ssh)) | Aucun |
| Sauvegarde ou restauration au niveau de la puce | Avec un programmateur de flash SPI et une pince SOIC-8, caméra ouverte | Facultatif pour l’installation. C’est le seul moyen de récupérer une caméra qui ne démarre plus |
| Couche média (v0.2.39, pas encore déroulée depuis le firmware d’origine) | Par Wi-Fi et SSH. Elle réécrit la partition applicative de la caméra | Aucun pour l’installation. En cas de coupure de courant pendant l’écriture, seuls un programmateur et une sauvegarde permettent de revenir en arrière |

**Pour installer la v0.2.21, vous n’avez besoin ni d’une carte microSD, ni d’un
câble USB, ni d’un adaptateur série, ni d’un programmateur de flash, et vous
n’avez pas à ouvrir la caméra.** Le câble Ethernet de la caméra n’est pas
utilisé dans ces instructions.

Une carte microSD n’est jamais nécessaire. Si une carte est insérée, le
firmware y écrit ses journaux de plantage. Dans son mode de démarrage de repli,
la couche média peut aussi exécuter ses fichiers programmes depuis une carte,
mais cela relève des mainteneurs et ne constitue pas une méthode d’installation
(voir [La couche média](#the-media-layer)).

## Ce dont vous avez besoin

- **La caméra.** Il doit s’agir d’une JOOAN W3-U dont l’étiquette ou
  l’application du fabricant indique le modèle `JA-A12`. D’autres caméras
  vendues sous le nom « W3-U » utilisent d’autres puces : lisez
  [Compatibilité](../../../docs/COMPATIBILITY.md). L’installateur vérifie le
  modèle et les fichiers du fabricant, et refuse toute caméra qui diffère. Il
  ne peut pas voir la carte électronique : ouvrir la caméra est donc la seule
  vérification complète (le document Compatibilité explique quoi vérifier).
- **L’application mobile JOOAN.** C’est par l’application que le firmware du
  fabricant rejoint votre réseau Wi-Fi. Ce projet n’a aucun autre moyen de
  mettre sur le réseau une caméra sous firmware d’origine.
- **Un routeur Wi-Fi** sur lequel :
  - la caméra peut rejoindre un réseau WPA2. Elle n’a pas pu se connecter à un
    point d’accès OpenWrt équipé d’une radio Qualcomm ath11k, alors que des
    points d’accès à base de Realtek ont fonctionné (voir
    [Compatibilité](../../../docs/COMPATIBILITY.md#wi-fi-access-point-interoperability)) ;
  - vous pouvez bloquer l’accès à Internet d’un seul appareil. Selon le
    routeur, cette fonction s’appelle contrôle parental, contrôle d’accès ou
    règle de pare-feu ;
  - votre PC se trouve sur le même réseau que la caméra.
- **Un PC** sous Windows 10 ou version ultérieure, macOS ou Linux, avec :
  - Python 3.9 ou plus récent ;
  - le paquet Python `cryptography` ;
  - un client SSH. Il est intégré à Windows 10 et versions ultérieures, à
    macOS et à Linux.
- **Environ 30 minutes** et une alimentation stable pour la caméra (5 V, 1,5 A).
  Ne débranchez pas la caméra pendant l’installation.

## Étape par étape

### 1. Configurer la caméra avec l’application JOOAN

Suivez la procédure de configuration habituelle de l’application et connectez
la caméra au réseau Wi-Fi sur lequel elle restera. Changer de réseau par la
suite est plus compliqué (voir l’[étape 8](#8-use-the-camera)). L’application
associe la caméra par l’intermédiaire du cloud de JOOAN : faites donc cette
étape tant que la caméra peut encore accéder à Internet, l’étape 3 bloquant
ensuite cet accès. (Le firmware du fabricant propose aussi un point d’accès de
configuration et une voie DHCP par Ethernet, qui permettraient peut-être de se
passer de l’application ; aucun des deux n’a été testé avec ce guide.)

Si l’application propose une mise à jour du firmware de la caméra, refusez-la.
Un firmware du fabricant plus récent modifie les fichiers que vérifie
l’installateur, qui refuse alors la caméra.

**C’est réussi si** l’application affiche la vidéo en direct.

### 2. Trouver l’adresse IP de la caméra

Consultez la liste des appareils connectés de votre routeur. Le firmware du
fabricant demande une adresse au routeur sous le nom `JAIPC`. L’adresse MAC de
la caméra figure dans les informations de l’appareil, dans l’application. Il
est utile de réserver cette adresse IP pour la caméra dans les réglages DHCP du
routeur, afin qu’elle ne change pas.

**C’est réussi si** la caméra répond à `ping CAMERA_IP` depuis votre PC. Ici et
dans la suite, remplacez `CAMERA_IP` par l’adresse trouvée, par exemple
`192.168.1.50`.

### 3. Bloquer l’accès de la caméra à Internet

Sur le routeur, bloquez l’accès à Internet de la caméra, et uniquement de la
caméra. Gardez votre PC sur le même réseau.

À chaque mise sous tension, pendant quelques secondes, le serveur web et
l’outil de mise à jour du fabricant s’exécutent avant que ce firmware ne prenne
le relais. Le blocage met cette fenêtre à l’abri d’Internet. Désormais,
l’application JOOAN ne peut plus joindre cette caméra. C’est normal.

**C’est réussi si** l’application indique que la caméra est hors ligne et que
celle-ci répond toujours à `ping CAMERA_IP`.

### 4. Préparer le PC

Téléchargez les éléments suivants dans un même dossier, créé pour l’occasion :

1. Le **code source de la v0.2.21**, depuis
   <https://github.com/ADCDS/jooan-w3u-local-firmware/archive/refs/tags/v0.2.21.zip>.
   Décompressez-le. Vous obtenez un dossier `jooan-w3u-local-firmware-0.2.21`.
2. Les six fichiers suivants, depuis la
   [page de la version v0.2.21](https://github.com/ADCDS/jooan-w3u-local-firmware/releases/tag/v0.2.21).
   Placez-les ensemble dans un dossier nommé `release`, à côté du dossier des
   sources : `JOOAN_FW_PKG`, `JOOAN_FW_PKG.sha256`, `JOOAN_UNINSTALL`,
   `JOOAN_UNINSTALL.sha256`, `manifest.json` et `manifest.json.sig`.

Les sources doivent être celles de la v0.2.21, et non celles de la branche
`main` actuelle : l’outil d’envoi actuel rejette le paquet v0.2.21 avec
l’erreur `signed release target/ABI contract does
not match this repository`.

Installez `cryptography`.

Linux ou macOS :

```sh
python3 -m pip install --user cryptography
```

Sous Debian ou Ubuntu, `sudo apt install python3-cryptography` fonctionne
également.

Windows (Invite de commandes) :

```bat
py -m pip install cryptography
```

**C’est réussi si** cette commande affiche un numéro de version :

```sh
python3 -c "import cryptography; print(cryptography.__version__)"
```

Sous Windows, tapez `py` à la place de `python3`, ici comme à l’étape 5.

### Livraison sans fil (OTA) : comment la caméra obtient réellement le paquet

Lisez cette section avant l’étape 5. L’outil de mise à jour d’origine
fonctionne par **récupération** (pull) depuis le cloud, et non par envoi
(upload). `tools/upload_ota.py` le déclenche, mais le contenu de sa requête
POST est ignoré : la caméra télécharge ensuite le firmware via TLS depuis une
URL fixe du cloud, `https://use1api.jooaniot.com/fw/JOOAN_FW_PKG`. Une
installation exige donc deux conditions, en plus de l’exécution de l’outil :

1. **Pour la caméra, `use1api.jooaniot.com` doit être résolu vers le PC qui
   servira le paquet.** C’est le point que la plupart des gens négligent. La
   caméra **ignore le serveur DNS que votre routeur distribue par DHCP** et
   interroge des résolveurs publics codés en dur (223.6.6.6 et 8.8.8.8).
   Modifier « votre DNS » ne sert à rien. La redirection doit se faire sur le
   chemin réseau. Deux méthodes fonctionnent :

   - **Un routeur qui remplace la résolution de ce nom pour la caméra.** Sous
     OpenWrt, une seule ligne dnsmasq, `address=/use1api.jooaniot.com/<PC-IP>`,
     plus une règle de pare-feu qui **redirige vers le résolveur du routeur le
     trafic que la caméra envoie elle-même sur le port 53** (un DNAT du trafic
     UDP/TCP 53 de la caméra vers le routeur), car sinon la caméra contourne le
     DNS du routeur. Limitez les deux à l’adresse MAC de la caméra.
   - **Un boîtier de redirection DNS sur le sous-réseau de la caméra**, qui
     répond à ce seul nom avec l’adresse du PC et transmet tout le reste, là
     encore avec le port 53 de la caméra redirigé vers lui.

   Inutile de simuler le reste du cloud : seul ce nom d’hôte est concerné, et
   uniquement pour le téléchargement du firmware.

2. **Le PC doit servir le paquet sur le port TCP 443.** `tools/serve_ota.py`
   fait exactement cela et rien d’autre : il répond à `/fw/JOOAN_FW_PKG` avec
   le fichier que vous lui indiquez, via un certificat auto-signé (la caméra ne
   le valide pas), et renvoie une erreur 404 pour tout autre chemin. Il ne se
   fait pas passer pour l’API du cloud, ne relaie rien vers l’amont et ne
   touche pas aux identifiants de l’appareil.

Si votre caméra fonctionne déjà sous jooan-local (cas d’une mise à jour
ultérieure, et non d’une première installation), rien de tout cela ne
s’applique : les mises à jour passent par l’API authentifiée `/api/v1/update`,
et non par la récupération depuis le cloud.

### 5. Installer

Commencez par servir le paquet. Dans le dossier
`jooan-w3u-local-firmware-0.2.21`, dans un premier terminal (des privilèges
sont nécessaires pour écouter sur le port 443 ; sous Linux, utilisez
`sudo`) :

```sh
sudo python3 tools/serve_ota.py --pkg ../release/JOOAN_FW_PKG
```

Il affiche `[ready] https://0.0.0.0:443/fw/JOOAN_FW_PKG` et génère un
certificat jetable pour `use1api.jooaniot.com`. Laissez-le tourner. Mettez en
place la redirection DNS décrite dans la section précédente pour que la caméra
atteigne ce serveur.

Ensuite, dans un second terminal ouvert dans le même dossier, déclenchez la
mise à jour sur la caméra :

```sh
python3 tools/upload_ota.py --host CAMERA_IP --pkg ../release/JOOAN_FW_PKG
```

L’outil vérifie la signature du paquet, déclenche l’outil de mise à jour du
fabricant sur la caméra (qui télécharge le paquet depuis le serveur lancé
ci-dessus), puis attend que la caméra redémarre avec le nouveau firmware. Le
terminal du serveur affiche `[served] /fw/JOOAN_FW_PKG -> ... bytes` lorsque la
caméra récupère le paquet. Comptez d’une à cinq minutes. Laissez la caméra sous
tension.

**C’est réussi si** la dernière ligne est :

```text
[+] jooan-local is active
```

En cas d’échec :

| Message | Signification et marche à suivre |
|---|---|
| `error: signed release target/ABI contract does not match this repository` | Vous exécutez une autre version des sources. Utilisez l’archive zip de la v0.2.21 obtenue à l’étape 4 |
| `... requires the Python 'cryptography' package` | Installez-le comme indiqué à l’étape 4 |
| `[-] unexpected OEM response HTTP ...` | L’outil de mise à jour du fabricant, sur la caméra, a refusé le déclenchement. Arrêtez-vous là |
| Le terminal du serveur n’affiche jamais `[served] ...` | La caméra n’a pas pu joindre votre serveur. La redirection DNS en est presque toujours la cause : vérifiez que `use1api.jooaniot.com` est résolu vers votre PC **du point de vue de la caméra** et que le port 53 de la caméra est bien redirigé (elle ignore le DNS fourni par DHCP). Vérifiez que le port 443 est ouvert sur le PC |
| `[-] package upload completed but jooan-local did not become healthy` | La caméra a accepté le paquet, mais ce firmware n’a pas démarré. Le plus probable est que l’installateur a trouvé des fichiers du fabricant différents et a refusé la caméra avant d’écrire quoi que ce soit. La caméra conserve son firmware du fabricant. Arrêtez-vous là et consultez [En cas de problème](#if-something-goes-wrong) |
| Aucune réponse | Vérifiez `CAMERA_IP`, et que le PC et la caméra sont bien sur le même réseau |

### 6. Se connecter

Ouvrez `http://CAMERA_IP/` dans un navigateur et connectez-vous :

```text
username: admin
password: admin
```

Le mot de passe reste `admin` jusqu’à ce que vous le changiez dans la section
System (Système). Le même mot de passe sert aussi pour SSH et RTSP : le
modifier le change donc pour les trois.

**C’est réussi si** `http://CAMERA_IP/api/v1/setup/status` affiche
`"release_version":"0.2.21"`.

<a id="7-back-up-the-camera-over-ssh"></a>
### 7. Sauvegarder la caméra via SSH

Recommandé. Cette opération enregistre sur votre PC une copie de chaque
partition de la mémoire flash, sans matériel supplémentaire. Seule cette copie
permet de récupérer cette caméra si une mise à jour ultérieure tourne mal : les
fichiers du fabricant qu’elle contient ne sont publiés nulle part. Restaurer
une caméra qui ne démarre plus nécessite en outre un programmateur de flash
(voir [Récupération](./RECOVERY.md#external-spi-recovery)).

La caméra compte sept partitions, de `mtd0` à `mtd6`. Cette commande les
affiche :

```sh
ssh admin@CAMERA_IP cat /proc/mtd
```

Enregistrez ensuite chaque partition. Lancez chaque commande et saisissez le
mot de passe lorsqu’il vous est demandé. Sous Windows, utilisez l’Invite de
commandes, et non PowerShell : l’opérateur `>` de Windows PowerShell
endommage les fichiers binaires.

```sh
ssh admin@CAMERA_IP "cat /dev/mtdblock0" > camera-mtd0.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock1" > camera-mtd1.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock2" > camera-mtd2.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock3" > camera-mtd3.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock4" > camera-mtd4.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock5" > camera-mtd5.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock6" > camera-mtd6.bin
```

**C’est réussi si** la somme des tailles des sept fichiers est de 8388608 octets
(8 Mio) et si, pour `camera-mtd4.bin` (la partition applicative), l’empreinte
MD5 calculée sur la caméra :

```sh
ssh admin@CAMERA_IP md5sum /dev/mtdblock4
```

correspond à celle calculée sur le PC : `md5sum camera-mtd4.bin` (Linux),
`md5 camera-mtd4.bin` (macOS) ou `certutil -hashfile camera-mtd4.bin MD5`
(Windows).

Ces fichiers contiennent l’identité de cette caméra, son mot de passe Wi-Fi et
ses clés. Gardez-les privés, ne les partagez jamais et ne les écrivez jamais
sur une autre caméra.

<a id="8-use-the-camera"></a>
### 8. Utiliser la caméra

Avec la v0.2.21 :

- **Vidéo :** RTSP sur le port 554, utilisateur `admin` et mot de passe
  administrateur. `rtsp://CAMERA_IP:554/live/ch0` correspond à l’objectif
  monté sur la tête panoramique/inclinaison et `rtsp://CAMERA_IP:554/live/ch1`
  à l’objectif fixe.
- **Panoramique/inclinaison et préréglages :** ONVIF à l’adresse `CAMERA_IP`,
  port 80, mêmes utilisateur et mot de passe.
- **SSH :** `ssh admin@CAMERA_IP`, même mot de passe.
- **Wi-Fi :** la caméra reste sur le réseau configuré par l’application. Pour
  changer de réseau, saisissez le nouveau réseau sous Network → Wi-Fi
  (Réseau → Wi-Fi) dans la console web, mettez-le à l’essai (Stage), puis
  validez-le (Commit staged) une fois la caméra reconnectée. Une modification
  qui n’est jamais validée est annulée.
- **Horloge :** la caméra n’a pas d’horloge sauvegardée par pile. Après une
  coupure de courant, appuyez sur « Sync time & zone to this device »
  (synchroniser l’heure et le fuseau horaire avec cet appareil) sous
  System → Camera time (Système → Heure de la caméra).

La documentation de la v0.2.21 décrit ces fonctionnalités. Consultez-la au
niveau du tag de cette version :
[README](https://github.com/ADCDS/jooan-w3u-local-firmware/blob/v0.2.21/README.md)
et [API](https://github.com/ADCDS/jooan-w3u-local-firmware/blob/v0.2.21/docs/API.md).
Les documents de la branche `main` décrivent la 0.2.39.

<a id="updating-later"></a>
## Mettre à jour par la suite

Installez une version publiée plus récente depuis la console web. Dans la
section Firmware, choisissez son fichier `JOOAN_FW_PKG`, attendez le message
« signature verified » (signature vérifiée), puis appliquez-le. La caméra
redémarre et reste hors ligne pendant environ 90 secondes. Une version n’est
acceptée que si elle est signée par ce projet et plus récente que celle qui
est installée.

**Ne mettez pas à jour une caméra en v0.2.21 vers la 0.2.22 ou une version
ultérieure si vous ne comptez pas installer la couche média juste après.** À
partir de la 0.2.22, la plateforme ne fournit plus elle-même la vidéo : tant
que la couche média n’est pas installée, la caméra n’a donc pas de vidéo.

Les versions postérieures à la 0.2.21 ne répondent qu’aux requêtes qui
désignent la caméra par une adresse IP privée ou par son propre nom. Ce nom
est `jooan-w3u`, sauf si vous le modifiez sous Network → Local name
(Réseau → Nom local). Il peut s’employer seul ou se terminer par `.local`,
`.lan`, `.home.arpa` ou `.internal`, comme dans `jooan-w3u.local` ou
`jooan-w3u.lan`. Toute requête adressée à un autre nom reçoit
`403 host_rejected` (voir
[Sécurité réseau](../../../docs/NETWORK-SECURITY.md#host-and-origin-checks)).

## Désinstaller

Dans la section Firmware, appliquez le fichier `JOOAN_UNINSTALL` de la version
que vous avez installée. Après le redémarrage, la caméra exécute de nouveau son
firmware du fabricant sans confinement : le serveur web du fabricant revient,
et la caméra peut de nouveau joindre le cloud, sauf si le routeur la bloque
toujours. La caméra conserve les réglages Wi-Fi propres au firmware du
fabricant. Les réglages de la plateforme, y compris un réseau Wi-Fi qui y a
été validé, sont supprimés. Les détails figurent dans
[Récupération](./RECOVERY.md#booting-camera-rollback).

**Si la couche média est installée, la désinstallation ne la supprime pas.**
Ses fichiers dans `/opt/jooanipc-open` restent en place, y compris le mot de
passe des flux et la clé privée TLS ; supprimez-les donc d’abord via SSH avec
`rm -rf /opt/jooanipc-open`. La partition applicative reste elle aussi dans sa
version réduite, sans les messages vocaux du fabricant ni les pilotes des
autres capteurs. **N’essayez pas de la réécrire sur une caméra en
fonctionnement :** le programme qui s’exécute depuis cette partition
plante au moment même où celle-ci est effacée, et un redémarrage survenant
avant la fin de l’écriture ne peut plus être rattrapé qu’avec un programmateur
de flash. Remettre la partition d’origine nécessite un programmateur de flash
et le fichier `my-appfs.img` que vous avez sauvegardé.

<a id="if-something-goes-wrong"></a>
## En cas de problème

- **La caméra démarre sur son firmware du fabricant après l’étape 5.** Si
  l’installateur a refusé la caméra, il n’a rien écrit et la caméra est
  inchangée. Vous pouvez revenir à l’application JOOAN en levant le blocage
  sur le routeur.
- **La console web ou SSH ne répond plus après une mise à jour.** Coupez puis
  rétablissez une fois l’alimentation de la caméra, et attendez trois minutes.
  Si elle ne répond toujours pas, suivez [Récupération](./RECOVERY.md).
- **La caméra ne démarre plus du tout.** Seule une restauration de votre
  sauvegarde au niveau de la puce, avec un programmateur de flash, permet de
  la récupérer ([Récupération](./RECOVERY.md#external-spi-recovery)).

Lorsque vous demandez de l’aide, ne publiez jamais d’images de la mémoire
flash, de mots de passe ni de clés (voir
[Politique de sécurité](../../../SECURITY.md#reporting-a-vulnerability)).

<a id="the-media-layer"></a>
## La couche média

À partir de la 0.2.22, la vidéo, ONVIF et le pilotage panoramique/inclinaison
sont assurés par le démon média ouvert [jooanipc-open](../../../media/), et non
plus par le programme du fabricant. Lors d’un démarrage natif, le mode par
défaut depuis la 0.2.23, le programme du fabricant ne se lance jamais. C’est
cette configuration que décrit le reste de ce dépôt.

**Toute cette chaîne a désormais été déroulée une fois, de bout en bout, sur
une vraie caméra.** Une caméra de test a été ramenée à son firmware d’origine
authentique, connectée au Wi-Fi sans l’application du fabricant, mise à jour
sans fil vers la plateforme, puis flashée avec une image média construite à
partir de sa propre partition. Elle a diffusé la vidéo en RTSP et a survécu à
un redémarrage à froid sur la pile logicielle ouverte. Voici ce que ce parcours
a vérifié, et ce qui reste encore peu étayé :

| Étape | Vérifié |
|---|---|
| Retour au firmware d’origine | La caméra de test a été reflashée avec la partition d’origine (chien de garde réarmé en continu, avec retour arrière en RAM) et a démarré sur un firmware d’origine authentique, non provisionné |
| Provisionnement sans l’application | Depuis un PC à proximité, une requête `GET /goform/setupWifiInfo` sur le point d’accès SoftAP de la caméra l’a connectée au Wi-Fi ; la caméra a signalé la réussite et a rejoint le réseau |
| Installation sans fil de la plateforme | Avec le cloud redirigé vers un serveur local, la caméra a récupéré puis appliqué le paquet signé de la plateforme, et a démarré dessus |
| L’image | Construite à partir de la vraie partition d’origine ; mêmes fichiers, à l’octet près, qu’une image média éprouvée |
| La configuration | Ce qu’écrit l’installateur passe avec succès la vérification intégrée au démon livré |
| L’installateur | Ses décisions, l’ordre des opérations et les vérifications préalables, à la fois sur une caméra simulée et sur la vraie caméra ; il a correctement refusé de poursuivre tant que la plateforme et le Wi-Fi n’étaient pas prêts |
| Le script de flashage, `first-appfs.sh` | Exécuté pour de vrai : il a flashé l’image média, conservé le retour arrière en RAM, et le démon ouvert a qualifié le démarrage natif et pris en charge le chien de garde |
| Résiste au redémarrage | Après un cycle complet de coupure et de remise sous tension, la caméra est revenue sur l’image média ouverte et a diffusé en RTSP, sans retour arrière vers le firmware d’origine |
| Installation de la 0.2.39 depuis le firmware d’origine | Réalisée avec le même outil de mise à jour au cours de ce parcours |

Deux défauts mineurs relevés au cours de ce parcours ont depuis été corrigés :

- Un réseau Wi-Fi **ouvert** (sans mot de passe) peut désormais être validé
  via la plateforme (la connexion utilise `key_mgmt NONE`) ; les réseaux WPA2
  fonctionnent comme avant.
- Sur une plateforme fraîchement installée, il peut manquer un peu de place
  dans la partition `/opt` de 384 Kio pour la première installation de la
  couche média. L’installateur supprime désormais automatiquement, pour
  libérer de la place, le fichier d’origine régénérable
  `/opt/conf/config.org` lorsque l’espace est insuffisant et que la copie
  d’origine permettant de le restaurer est présente (le démarrage d’origine
  recrée ce fichier à partir de `/mnt/mtd/etc`).

Si la relecture de contrôle qui suit le flashage échoue, le script restaure la
partition à partir d’une copie en RAM. **Si l’alimentation est coupée pendant
l’écriture, seuls un programmateur de flash et une sauvegarde de cette
partition permettent de récupérer la caméra** (voir
[Récupération](./RECOVERY.md#appfs-reflash-recovery)). Le flashage a désormais
été mené à son terme sur une vraie caméra, mais une seule fois : faites
d’abord une sauvegarde complète de toutes les partitions (l’installateur s’en
charge) et maintenez la caméra sur une alimentation stable pendant les quelques
minutes que dure l’opération.

Il vous faut un ordinateur sous Linux ou WSL, équipé de `squashfs-tools`,
Python 3, OpenSSH et OpenSSL, ainsi que les fichiers de la v0.2.39 disponibles
sur la page de la version.

1. **Installez la plateforme.** Suivez les étapes 4 et 5 avec les sources et
   les fichiers de la v0.2.39 ou, depuis la v0.2.21, effectuez la mise à jour
   comme décrit dans [Mettre à jour par la suite](#updating-later). La caméra
   n’a alors plus de vidéo jusqu’à la dernière étape. La console web reste
   accessible à l’adresse `http://CAMERA_IP/`.
2. **Sauvegardez la caméra** ([étape 7](#7-back-up-the-camera-over-ssh)).
3. **Validez son réseau Wi-Fi** sous Network → Wi-Fi (Réseau → Wi-Fi) dans la
   console web, même s’il s’agit du réseau auquel la caméra est déjà connectée.
   Un démarrage natif n’utilise que le réseau validé à cet endroit, et non
   celui configuré par l’application JOOAN.
4. **Décompressez** `jooanipc-open-*.tar.gz` et `libimp-*.tar.gz` côte à côte,
   puis ouvrez un terminal dans le dossier `jooanipc-open-*`.
5. **Copiez la partition applicative de votre caméra, puis construisez votre
   image à partir de celle-ci.** La copie comme l’image contiennent des
   fichiers du fabricant ; gardez-les donc privées :

   ```sh
   ssh admin@CAMERA_IP 'cat /dev/mtdblock4' > my-appfs.img
   sh media/tools/build-appfs.sh --first my-appfs.img bin/jooanipc-open ../libimp-*/libimp.so bin/libatomic.so.1 my-bundle
   ```

   Le script refuse une partition à laquelle manque un fichier dont il a
   besoin, ou qui contient un programme du fabricant différent de celui
   qu’impose la plateforme.

6. **Vérifiez la caméra.** Cette commande ne modifie rien :

   ```sh
   CAMERA_PASSWORD=admin python3 media/scripts/install-camera.py my-bundle --camera CAMERA_IP
   ```

7. **Installez,** en choisissant le mot de passe des flux vidéo de la caméra :

   ```sh
   CAMERA_PASSWORD=admin STREAM_PASSWORD=your-choice python3 media/scripts/install-camera.py my-bundle --camera CAMERA_IP --yes-untested
   ```

   Laissez la caméra sous tension. **C’est réussi si** la dernière ligne
   indique que la version est en cours d’exécution. L’interface
   d’administration se trouve désormais à l’adresse `https://CAMERA_IP/`, avec
   le propre certificat de la caméra : le navigateur affiche donc un
   avertissement la première fois. Connectez-vous avec le mot de passe
   administrateur. `http://CAMERA_IP/` se contente désormais de rediriger vers
   cette adresse.

Le démarrage se contrôle lui-même : un démarrage natif qui enregistre un échec,
ou qui est interrompu trois fois de suite, fait se replier le démarrage suivant
sur le programme du fabricant. Il en va de même d’un démarrage sans passerelle
au bout de 5 minutes, tant qu’aucun démarrage natif n’a encore été qualifié
(voir [Récupération](./RECOVERY.md#a-media-change-that-stops-native-boot)).

**Sous Windows,** construisez le bundle dans WSL, puis lancez l’installateur
avec `py` dans l’Invite de commandes, en utilisant `set` pour chaque mot de
passe. Il utilise l’OpenSSH fourni avec Windows et a besoin de la commande
`openssl` dans le PATH, que fournit Git for Windows. OpenSSH 8.4 ou ultérieur
(Windows 11) lit le mot de passe dans `CAMERA_PASSWORD` ; la version de
Windows 10 le demande à chaque connexion : ajoutez donc d’abord une clé SSH
dans la console web. Cette procédure n’a pas non plus été exécutée sous
Windows.

**Les mises à jour ultérieures de la couche média** partent de la partition
actuelle de votre caméra, avec `build-appfs.sh` sans `--first` et un fichier
`bundle.json` que vous rédigez pour l’occasion (voir la section
[Bundles de version de la caméra](../../../media/docs/hardware.md#camera-release-bundles)).
L’installateur a alors aussi besoin de `MEDIA_PASSWORD`, le mot de passe des
flux, pour vérifier le résultat. Le formulaire Système → Média
(System → Media) de l’interface d’administration n’accepte que les paquets
signés avec la clé de publication de ce projet. Chaque mise à jour de la couche
média, qu’elle passe par l’installateur ou par ce formulaire, réécrit la même
partition que la première installation : l’avertissement ci-dessus concernant
les coupures d’alimentation s’y applique donc aussi, réinitialisations de la
carte elle-même comprises. Comme une image contient des fichiers du fabricant,
aucune n’est publiée : ce formulaire s’adresse donc au détenteur de la clé ou à
quiconque construit la plateforme avec sa propre clé.

**Carte SD :** le mode de démarrage de repli de la couche média peut exécuter
le démon depuis une carte microSD au lieu de la partition applicative. Seul le
programme du fabricant monte la carte : cela ne fonctionne donc que dans ce
mode de repli. Cela ne permet de contourner aucune des étapes ci-dessus.

Le format d’un bundle est décrit dans la section
[Bundles de version de la caméra](../../../media/docs/hardware.md#camera-release-bundles).
