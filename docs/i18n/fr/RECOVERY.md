<!-- Translated from docs/RECOVERY.md. Authoritative version: ../../RECOVERY.md -->

# Récupération

## Niveaux de récupération

Deux voies indépendantes sont nécessaires :

- **Retour arrière ou désinstallation par paquet** pour une caméra qui démarre
  et dont la gestion locale ou le canal de mise à jour OEM fonctionne encore.
- **Récupération SPI externe** pour une caméra qui ne démarre plus ou qui est
  injoignable.

Le reflashage de l’appfs par la couche média ajoute une troisième voie,
manuelle, pour le seul cas où il maintient la caméra en marche sur une
partition endommagée ; voir
[Récupération après un reflashage de l’appfs](#appfs-reflash-recovery).

Un paquet de désinstallation stocké uniquement sur la caméra ne constitue pas
une méthode de récupération indépendante. Conservez-le hors de l’appareil. La
restauration SPI externe reste la voie la plus robuste pour une caméra qui ne
peut ni démarrer ni accepter de mise à jour.

## Éléments de récupération

Conservez les éléments suivants hors ligne, étiquetés pour une seule et même
caméra physique :

- deux lectures complètes de la flash de 8 Mio, identiques octet pour octet,
  avec leurs empreintes SHA-256, réalisées via SSH une fois la plateforme en
  service
  ([Installation, étape 7](./INSTALL.md#7-back-up-the-camera-over-ssh)) ou
  avec un programmateur SPI ;
- des photos du PCB `JA-6621 V1.0`, de l’orientation de la flash, de
  l’orientation de la pince et du câblage ;
- le modèle du programmateur, la tension réglée, la référence de flash
  sélectionnée et une commande de lecture éprouvée ;
- les paquets d’installation et de désinstallation exacts, leurs manifestes et
  leurs empreintes ;
- une lecture de la partition appfs (`/dev/mtd4`) réalisée avant chaque mise
  à jour de la couche média ; le répertoire `backups/` du bundle ne contient
  que des fichiers de configuration ;
- une copie des sources de la version et des métadonnées de la chaîne de
  compilation ;
- des notes expurgées décrivant le dernier démarrage réussi et l’état du
  réseau.

Ne partagez jamais le dump d’une caméra comme image de récupération pour un
autre appareil.

<a id="booting-camera-rollback"></a>
## Retour arrière sur une caméra qui démarre

1. Placez la caméra et le poste de travail sur le réseau de récupération isolé.
2. Bloquez au niveau du routeur tout accès de la caméra à Internet.
3. Vérifiez l’empreinte du paquet de désinstallation et ses métadonnées de
   cible `A12`.
4. Passez par la voie de mise à jour locale authentifiée ou par le canal hors
   ligne documenté.
5. Maintenez une alimentation stable jusqu’à ce que le paquet signale la fin de
   l’opération et que le redémarrage soit terminé.
6. Effectuez un démarrage à froid, examinez l’état et les services en écoute,
   puis vérifiez que l’état restauré est bien celui attendu.

La désinstallation conserve les données d’usine et la configuration Wi-Fi
enregistrée par le firmware OEM, mais supprime l’état de la plateforme, y
compris le réseau Wi-Fi validé (`wifi.json`) ; l’application OEM utilise
ensuite son propre réseau enregistré, qui n’est pas forcément celui
qu’utilisait la caméra. Si la caméra ne parvient pas à s’authentifier, ne peut
pas valider le paquet ou redémarre en boucle, cessez toute tentative de mise à
jour par le réseau et passez à la récupération externe.

Le remplacement du runtime est une transaction de maintenance : le contrôleur
signé et le bundle de récupération SSH restent disponibles pendant que
l’unique runtime persistant est remplacé. Pendant les deux minutes de l’essai
de bon fonctionnement, le superviseur applique le réseau Wi-Fi validé (un
démarrage natif qui n’a rien à attendre renonce à sa passerelle au bout
de 300 s) et relance un candidat dont le démarrage a échoué : `start.sh` est
limité à 15 s, mais `entropy-ready.sh` attend jusqu’à 30 s, et l’entropie
s’accumule lentement au début d’un démarrage natif. Si le candidat échoue
malgré tout, ou si l’essai est interrompu, il reste sélectionné comme unique
runtime, puisque la mise à jour a déjà supprimé le précédent, et le
superviseur continue de le relancer ; reprenez la main via le SSH géré par le
contrôleur, puis réessayez ou désinstallez. (Avant que ce défaut ne soit
corrigé, un essai échoué supprimait le candidat et ne laissait aucun runtime ;
le paragraphe suivant décrit cet état.) L’ancien runtime peut continuer de
tourner depuis tmpfs jusqu’au redémarrage qui conclut la mise à jour, mais il
ne constitue pas un emplacement de retour arrière sur la flash. Une mise à
jour de la plateforme qui échoue ne se replie pas sur l’outil de mise à jour
OEM : GoAhead continue d’être tué tant que le contrôleur fonctionne.

Sans runtime, rien n’applique le réseau Wi-Fi validé. Un démarrage natif ne
peut alors pas joindre sa passerelle, constate que rien ne tente de le faire,
échoue au bout de 300 s et pose le marqueur `native-failed` ; les démarrages
suivants exécutent l’application OEM confinée, qui rejoint son propre réseau
enregistré. Sur l’appareil vérifié, il s’agit du réseau de banc d’essai du
fabricant, `JOOANTEST` (ouvert, recherché activement comme SSID masqué). Pour
joindre la caméra dans cet état :

1. Créez un réseau `JOOANTEST` temporaire, masqué et ouvert, sur un point
   d’accès à portée de la caméra, réservé à l’adresse MAC de celle-ci et placé
   sur un sous-réseau isolé. La caméra s’y connecte en quelques secondes. Elle
   ne prend pas forcément d’adresse IPv4, mais son adresse IPv6 lien-local
   répond en SSH (dropbear écoute sur `[::]:22`), par exemple via
   `ssh -L 2222:[fe80::…%<bridge>]:22` lancé sur ce point d’accès.
2. Installez le runtime comme emplacement stable : copiez `runtime.tar.gz` et
   `runtime.md5` depuis le répertoire de préparation (stage) de la version vers
   `/opt/custom/jooan-local/slots/A/`, puis, après avoir chargé `common.sh`
   dans le shell, exécutez `jl_lock; jl_write_selection A - 0; jl_unlock`.
3. Supprimez `/opt/jooanipc-open/native-failed`, exécutez `sync` et redémarrez.
   Le superviseur démarre le runtime (et le relance si son premier démarrage
   échoue), le hook de celui-ci applique le réseau validé, et le démarrage
   natif est qualifié.
4. Supprimez le réseau temporaire.

## Une mise à jour s’est arrêtée

Sur une caméra à court de place, une mise à jour de la plateforme supprime
l’unique runtime présent sur la flash avant d’écrire le nouveau. Si elle
s’arrête entre les deux, la caméra continue de fonctionner depuis la RAM, mais
la flash ne contient plus rien à lancer au prochain démarrage : la page Système
indique que la mise à jour s’est arrêtée, et tout redémarrage est refusé avec
`no-runtime`.

**Ne redémarrez pas et ne débranchez pas encore la caméra.** Le démarrage
suivant n’aurait pas de réseau, et il ne vous resterait plus que la procédure
ci-dessus. Via SSH :

```sh
cat /opt/custom/jooan-local/state/selection
ls /opt/custom/jooan-local/slots/
```

Si la sélection désigne un emplacement (`A - 0`), que cet emplacement existe
bien et que `df -k /opt` indique au moins 51 Kio libres, rien n’est perdu. En
dessous de 50 Kio au démarrage, le `startapp` du fabricant efface `/opt/etc`
et `/opt/jooanipc-open`, ce qui est pire que de ne pas avoir de runtime :
libérez donc d’abord de la place. Ce qui peut être supprimé : tout ce qui porte
un nom en `*.new`, `*.next` ou `*.previous` sous `/opt/custom/jooan-local` et
`/opt/jooanipc-open`, ainsi que `state/net-loss.log`. Ce qui ne doit en aucun
cas être supprimé : `config/`, `state/selection`, `controller.tar.gz`,
`recovery/`, un emplacement sélectionné et, dans `/opt/jooanipc-open`,
`camera.ini`, `deployment.env`, les deux scripts de démarrage et les clés. Si
la sélection indique `- - 0`, remettez un runtime en place. Arrêtez d’abord
tout installateur encore en attente sur cette caméra : il reprendrait son
travail dès qu’il verrait un redémarrage.

1. Retrouvez le paquet en échec, conservé en RAM jusqu’au prochain
   redémarrage :
   ```sh
   ls /run/jooan-local/firmware.*.sqfs
   mkdir /tmp/fw && mount -t squashfs -o ro,loop /run/jooan-local/firmware.<id>.sqfs /tmp/fw
   ```
   S’il a disparu, les deux mêmes fichiers se trouvent dans le `JOOAN_FW_PKG`
   de la version. Sur votre ordinateur, exécutez
   `dd if=JOOAN_FW_PKG bs=96 skip=1 of=p.sqfs && unsquashfs p.sqfs runtime.tar.gz runtime.md5`,
   puis `mkdir -p /tmp/fw` sur la caméra, et copiez-les dans ce répertoire
   avec `ssh admin@CAMERA 'cat > /tmp/fw/runtime.tar.gz' < squashfs-root/runtime.tar.gz`.
2. Copiez le runtime dans un emplacement, et laissez la somme de contrôle
   trancher. Vérifiez d’abord que `df -k /opt` indique au moins la taille de
   `runtime.tar.gz` plus 60 Kio ; sinon, libérez de la place, pour la raison
   exposée plus haut.
   ```sh
   cd /opt/custom/jooan-local/slots
   rm -rf A.new
   mkdir A.new && cp /tmp/fw/runtime.tar.gz /tmp/fw/runtime.md5 A.new/
   [ "$(md5sum A.new/runtime.tar.gz | cut -c1-32)" = "$(cat A.new/runtime.md5)" ] && sync && mv A.new A && sync
   ```
3. Sélectionnez cet emplacement à l’aide des propres fonctions du contrôleur
   en cours d’exécution :
   ```sh
   . /run/jooan-local/controller/boot/common.sh; jl_lock; jl_write_selection A - 0; jl_unlock
   ```

Le superviseur ne lit la sélection qu’au démarrage : cette opération ne
perturbe donc rien. Si `df -k /opt` indique au moins 51 Kio, la caméra peut
désormais redémarrer sans risque, et la mise à jour peut être retentée. La
page d’administration garde les commandes Redémarrer et Réinitialiser
verrouillées jusqu’à ce que la caméra ait redémarré : redémarrez-la donc depuis
la même session SSH avec `jl_reboot_asked`, qui sauvegarde d’abord l’heure et
signale au démarrage suivant que le redémarrage a été demandé. Cette procédure
a été réellement appliquée sur camera1 le 2026-11-07, depuis la RAM.

<a id="a-media-change-that-stops-native-boot"></a>
## Une modification de la couche média qui empêche le démarrage natif

Si le démon média se termine pendant un démarrage natif, son chien de garde
armé réinitialise la carte. Un démarrage qui a enregistré l’échec conduit le
suivant à poser le marqueur `native-failed` et à passer par le transfert
(handoff) ; un démarrage simplement interrompu, par une coupure de courant ou
un blocage, a d’abord droit à deux nouvelles tentatives. Un transfert qui
échoue lui aussi pose le marqueur `boot-trial` et laisse tourner l’application
OEM confinée. Un redémarrage demandé par la plateforme n’est pas retenu contre
le démarrage qu’il a interrompu, mais il ne neutralise pas pour autant un
échec enregistré avant lui.

Un démarrage natif sans passerelle attend, sans réinitialisation ni marqueur,
tant que l’attente a une chance d’aboutir : un réseau validé, un runtime qui
tente de le rejoindre et un démarrage antérieur qualifié sur cette caméra.
Sinon, il échoue au bout de 300 s, comme décrit plus haut. Une version qui perd
la passerelle après son démarrage est tout de même qualifiée, sauf si cela se
produit sur trois démarrages consécutifs. Si un réseau validé a définitivement
disparu, une caméra déjà qualifiée reste en attente : pour la joindre, recréez
un réseau portant ce nom et ce mot de passe. La plateforme et SSH répondent
toujours, mais pas l’interface HTTPS. Pour récupérer la caméra :

1. Lisez la cause de l’échec dans `/tmp/jooanipc-open.log`, ou dans un journal
   répliqué hors de la caméra si la carte a été réinitialisée depuis.
   Recherchez `startup failed` et `health check` avec grep, ainsi que
   `Process ` dans les messages du noyau : un oops signifie que la cause ne
   vient peut-être pas de la version.
2. Annulez la cause, s’il s’agit d’un réglage enregistré. Par exemple, dans
   `/opt/jooanipc-open/state/controls.json` ou `/opt/jooanipc-open/camera.ini`.
3. Exécutez `cd /opt/jooanipc-open && rm -f native-failed native-trial native-retry boot-trial && sync`
   puis redémarrez. Le démarrage repasse en mode natif, et l’installateur
   accepte la caméra.

C’est arrivé deux fois sur la caméra de test le 2026-11-06, avec une version
qui ne pouvait pas démarrer lorsque le mode privé était activé. Elle a été
récupérée de cette manière en 5 minutes environ à chaque fois.

C’est arrivé une troisième fois le même jour avec une version saine :
`IMP_ISP_Open failed (-1)`. Un script de surveillance lisait
`/proc/jz/isp/isp-m0` via SSH pendant que la carte démarrait. Lu après le
chargement du module ISP et avant que le démon n’ait ouvert l’ISP, ce fichier
provoque un oops dans le pilote (`Process awk` dans les messages du noyau), et
l’ouverture par le démon, une seconde plus tard, a échoué. Un démarrage natif
reste dans cet état pendant 30 s ou plus, le temps d’attendre le Wi-Fi. Tout
outil qui lit périodiquement ce fichier doit d’abord constater `"ready":true`
dans `/api/v1/status`.

La désinstallation signée est une suppression destructive, et non une
restauration de l’état antérieur. Elle retire la console web, l’élagage des
routes, le garde-fou des processus, le SSH du projet ainsi que le contrôleur et
le runtime compressés ; le démarrage suivant revient au comportement
OEM/GoAhead, avec l’application OEM exécutée sans confinement. Les fichiers de
la couche média dans `/opt/jooanipc-open` et sa version dans l’appfs restent
sur la flash, mais plus rien ne les lance une fois `local.rc` supprimé. La
désinstallation ne restaure ni l’ancien hook telnet non authentifié, ni un
`/opt/etc/local.rc` antérieur archivé. Maintenez l’isolation au niveau du
routeur avant, pendant et après la désinstallation.

<a id="appfs-reflash-recovery"></a>
## Récupération après un reflashage de l’appfs

`media/scripts/upgrade-appfs.sh` reflashe `/dev/mtd4`, l’appfs (partition
applicative) : elle contient le `startapp` OEM, `jooanipc`, les modules du
noyau, les réglages IQ des capteurs et la version de la couche média. Avant
d’arrêter le démon média, le script prend le verrou de maintenance de la
plateforme (`/run/jooan-local/state.lock`), ce qui suspend les relances du
runtime par le superviseur ainsi que le redémarrage déclenché par le chien de
garde réseau, et un processus `watchdog` du rootfs prend le relais pour
rafraîchir le chien de garde matériel armé. La section Système → Média de
l’interface d’administration exécute ce même script.
`media/scripts/first-appfs.sh`, destiné à la première version média d’une
caméra, suit les mêmes étapes en partant de l’application d’origine. Son
retour arrière supprime aussi la configuration média : la caméra revient alors
à son état d’origine, avec la plateforme seule.

- **Avant le point de validation** (la bascule de configuration juste avant
  `flashcp`), tout échec entraîne un redémarrage sur l’appfs intacte.
- **Après ce point**, un flashage dont la vérification échoue est annulé à
  partir d’une copie en RAM de l’appfs d’origine, en trois tentatives au
  plus ; les fichiers de configuration et `native-boot.sh` sont ensuite
  restaurés, et la caméra redémarre.
- **Si le retour arrière échoue lui aussi**, l’installateur ne redémarre pas.
  La caméra reste en marche, avec le processus de rafraîchissement du rootfs
  actif, le verrou maintenu et les deux images dans `/tmp` :
  `/tmp/jipc-appfs-rollback.bin` (l’appfs d’origine) et
  `/tmp/jipc-flash-candidate.squashfs` (la nouvelle image). SSH continue de
  fonctionner, car la plateforme s’exécute depuis `/run` et le rootfs.

Remédiez à cet état via SSH, sans redémarrer :

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

Une première installation ne conserve aucun fichier `.before`, puisqu’il n’y
avait pas encore de couche média. Après le `flashcp` et la vérification, au
lieu des trois lignes `cp` :

```sh
cd /opt/jooanipc-open && rm -f boot-handoff.sh camera.ini deployment.env enabled native
```

L’empreinte MD5 de l’appfs d’origine est le deuxième argument transmis à
l’installateur : le champ `expected_current_appfs_md5` du bundle, que
`install-camera.py` affiche également lors des vérifications préalables.
Relancez `flashcp` jusqu’à ce que `md5sum` corresponde ; ne redémarrez surtout
pas avant. `/tmp` réside en RAM : les images sont perdues au moindre
redémarrage.

Si l’alimentation est coupée, ou si la caméra redémarre, alors que l’appfs est
endommagée, `startapp` est absent : plus rien ne charge `local.rc`, si bien que
ni la plateforme ni SSH ne démarrent. La récupération exige alors un
programmateur de flash SPI (SPI NOR de 8 Mio) et une sauvegarde de la
partition appfs ou de la flash entière, comme décrit ci-dessous.

<a id="external-spi-recovery"></a>
## Récupération SPI externe

La carte vérifiée utilise une flash SPI NOR de 8 Mio (Puya P25Q64HA sur
l’appareil observé). Vérifiez quelle puce est effectivement montée plutôt que
de le présumer.

1. Débranchez l’alimentation et le câble Ethernet de la caméra.
2. Utilisez un programmateur compatible 3,3 V et vérifiez la broche 1 et
   l’orientation d’après la carte et la fiche technique de la flash.
3. Relisez la puce avant toute écriture et conservez cette image de l’état
   défaillant.
4. Vérifiez que la sauvegarde fait exactement 8 Mio et que son empreinte
   SHA-256 correspond à celle de l’une des lectures effectuées avant
   l’installation.
5. Écrivez l’image complète avec la commande vérifiée du programmateur.
6. Relisez la puce et comparez chaque octet avec la sauvegarde.
7. Déconnectez le programmateur avant de remettre la caméra sous son
   alimentation normale.
8. Ne démarrez la caméra que sur le réseau de récupération isolé, puis
   vérifiez le noyau OEM, les deux capteurs, la radio et le comportement
   réseau.

Les commandes propres à chaque programmateur ne sont volontairement pas
généralisées ici : le câblage de la pince, la tension du contrôleur et la prise
en charge des puces flash varient. Suivez la documentation du programmateur et
celle du fabricant de la flash. N’appliquez jamais 5 V à une flash SPI NOR
prévue pour 3,3 V.

## Après la récupération

Considérez comme potentiellement compromis tous les identifiants et toutes les
clés privées présents lors d’une défaillance de sécurité. Changez le mot de
passe administrateur, remplacez les clés SSH enregistrées et remplacez les
identifiants Wi-Fi. Vérifiez de nouveau l’isolation au niveau du routeur avant
de remettre la caméra en service normal.

Notez pourquoi une récupération a été nécessaire et conservez des journaux
expurgés. Ne publiez dans un ticket (issue) ni images brutes de la flash, ni
clés, ni jetons, ni cookies, ni configuration Wi-Fi.

## Une adresse fixe sur le mauvais réseau

Une adresse fixe est conservée après un redémarrage, et un nouveau routeur peut
se trouver sur un autre sous-réseau. La réinitialisation aux valeurs par défaut
repasse l’adresse en automatique et conserve le réseau Wi-Fi : maintenir le
bouton de réinitialisation enfoncé pendant 5 secondes permet donc de revenir en
arrière. Si le bouton est inaccessible : installez un routeur temporaire sur
l’ancien sous-réseau, avec l’adresse de l’ancienne passerelle, car le démon
média ne démarre qu’une fois que celle-ci répond. Rendez-vous ensuite dans
l’interface d’administration, sous Réseau.

## La récupération n’est terminée que lorsque

- l’appareil réussit plusieurs démarrages à froid successifs ;
- les deux chaînes CV2005 produisent la vidéo attendue ;
- l’interface SKW6316 est stable ;
- la voie de gestion prévue fonctionne ;
- les ports inattendus et le trafic sortant inattendu sont absents ou bloqués ;
- la nouvelle lecture complète de la flash est archivée avec son empreinte.
