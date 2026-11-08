<!-- Translated from docs/INSTALL.md. Authoritative version: ../../INSTALL.md -->

# Instalação

Este guia é para quem tem uma câmera JOOAN W3-U e sabe seguir instruções e
copiar comandos, mas não é especialista em Linux. Mantenedores que geram as
próprias versões devem consultar [Compilação e publicação](../../../docs/BUILDING.md).

<a id="read-this-first"></a>
## Leia isto primeiro

Até 2026-11-07, duas versões foram publicadas no
[GitHub Releases](https://github.com/ADCDS/jooan-w3u-local-firmware/releases):

- **v0.2.21** (2026-10-16) é a versão descrita passo a passo neste guia e a última
  já instalada em uma câmera com firmware original. Ela é instalada por Wi-Fi, sem
  hardware adicional. Você passa a ter um console web local, SSH, vídeo RTSP das duas
  lentes, além de pan/tilt e presets via ONVIF para um NVR como o Frigate, e a
  câmera deixa de ter acesso à nuvem da JOOAN. O vídeo ainda vem do programa de
  câmera do fabricante, que continua rodando sob confinamento.
- **v0.2.39** (2026-11-07) acrescenta o daemon de mídia aberto em
  [`media/`](../../../media/) e é o que o restante deste repositório descreve.
  **Nenhuma câmera passou ainda do firmware original para a v0.2.39 seguindo
  estas instruções.** Leia [A camada de mídia](#the-media-layer), inclusive o
  que foi e o que não foi testado, antes de escolhê-la.
- **Cada uma só é instalada em uma câmera cujo firmware do fabricante seja igual ao da
  unidade verificada.** Antes de gravar qualquer coisa, o instalador compara os arquivos
  do fabricante com os da unidade verificada: a v0.2.21, a partição de aplicação inteira;
  a v0.2.39, os doze arquivos de que depende. Se divergirem, ele recusa a instalação e deixa a
  câmera como estava. A camada de mídia exige ainda mais da sua partição de aplicação: cada
  arquivo que ela mantém precisa estar lá, e a câmera precisa ter os sensores CV2005.
  Ninguém sabe ainda se as câmeras vendidas hoje vêm com o mesmo firmware do fabricante.
- Nenhuma versão tem "suporte" ainda. Todas as versões até agora são
  pré-lançamentos de engenharia, testados em uma única câmera.

## Via OTA ou por hardware?

| O quê | Como | Hardware adicional |
|---|---|---|
| Primeira instalação | Por Wi-Fi, pelo atualizador web integrado ao firmware do fabricante | Nenhum |
| Atualizações posteriores e desinstalação | Por Wi-Fi, pela seção Firmware do console web | Nenhum |
| Backup da flash da câmera | Por SSH, após a instalação ([passo 7](#7-back-up-the-camera-over-ssh)) | Nenhum |
| Backup ou restauração no nível do chip | Com um gravador de flash SPI e uma garra SOIC-8, com a câmera aberta | Opcional para a instalação. É a única forma de recuperar uma câmera que não inicializa mais |
| Camada de mídia (v0.2.39, ainda não executada a partir do firmware original) | Por Wi-Fi e SSH. Ela regrava a partição de aplicação da câmera | Nenhum para instalar. Se faltar energia durante a gravação, a única forma de recuperar a câmera é com um gravador de flash e um backup |

**Para instalar a v0.2.21, você não precisa de cartão microSD, cabo USB, adaptador
serial nem gravador de flash, e também não precisa abrir a câmera.** O cabo
Ethernet da câmera não é usado nestas instruções.

Um cartão microSD nunca é necessário. Quando há um cartão inserido, o firmware grava
nele os logs de falhas. A camada de mídia também pode executar seus arquivos de programa a
partir de um cartão no modo de inicialização de reserva, mas isso é assunto para
mantenedores, não um caminho de instalação (veja [A camada de mídia](#the-media-layer)).

## Do que você precisa

- **A câmera.** Ela precisa ser uma JOOAN W3-U cuja etiqueta ou cujo aplicativo do
  fabricante indique o modelo `JA-A12`. Outras câmeras vendidas como "W3-U" usam
  chips diferentes: leia [Compatibilidade](../../../docs/COMPATIBILITY.md). O instalador
  verifica o modelo e os arquivos do fabricante e recusa uma câmera diferente. Ele não
  tem como ver a placa de circuito, então abrir a câmera é a única verificação completa
  (o guia de Compatibilidade explica o que procurar).
- **O aplicativo JOOAN para celular.** É por ele que o firmware do fabricante entra
  no seu Wi-Fi. Este projeto não tem outro meio de colocar na rede uma câmera com firmware original.
- **Um roteador Wi-Fi** em que:
  - a câmera consiga entrar em uma rede WPA2. Não foi possível conectá-la a um ponto de
    acesso OpenWrt com rádio Qualcomm ath11k. Pontos de acesso baseados em Realtek
    funcionaram (veja [Compatibilidade](../../../docs/COMPATIBILITY.md#wi-fi-access-point-interoperability));
  - seja possível bloquear o acesso de um único dispositivo à Internet. Os roteadores
    chamam isso de controle parental, controle de acesso ou regra de firewall;
  - o seu PC esteja na mesma rede que a câmera.
- **Um PC** rodando Windows 10 ou posterior, macOS ou Linux, com:
  - Python 3.9 ou mais recente;
  - o pacote Python `cryptography`;
  - um cliente SSH. Ele já vem integrado ao Windows 10 e posteriores, ao macOS e ao Linux.
- **Cerca de 30 minutos** e uma fonte de alimentação estável para a câmera (5 V, 1,5 A).
  Não desligue a câmera da tomada enquanto a instalação estiver em andamento.

## Passo a passo

### 1. Configure a câmera com o aplicativo JOOAN

Siga a configuração normal do aplicativo e conecte a câmera à rede Wi-Fi em que ela
vai ficar. Mudar a rede da câmera depois é mais difícil (veja o
[passo 8](#8-use-the-camera)). O aplicativo pareia a câmera pela nuvem da JOOAN,
então faça isso enquanto a câmera ainda consegue acessar a Internet; o passo 3
bloqueia esse acesso depois. (O firmware do fabricante também tem um hotspot de
configuração e um caminho via DHCP pela Ethernet, que talvez dispensem o aplicativo;
nenhum dos dois foi testado com este guia.)

Se o aplicativo oferecer uma atualização de firmware da câmera, recuse. Um firmware
do fabricante mais recente altera os arquivos que o instalador verifica, e então o
instalador recusa a câmera.

**Funcionou se** o aplicativo mostrar vídeo ao vivo.

### 2. Descubra o endereço IP da câmera

Procure na lista de dispositivos conectados do seu roteador. O firmware do fabricante
pede um endereço ao roteador identificando-se como `JAIPC`. O endereço MAC da câmera aparece
nas informações do dispositivo no aplicativo. Vale a pena reservar o endereço da câmera
nas configurações de DHCP do roteador, para que ele não mude.

**Funcionou se** a câmera responder a `ping CAMERA_IP` do seu PC. Aqui e no restante
do guia, substitua `CAMERA_IP` pelo endereço que você encontrou, como `192.168.1.50`.

### 3. Bloqueie o acesso da câmera à Internet

No roteador, bloqueie o acesso à Internet da câmera, e somente da câmera. Mantenha
o seu PC na mesma rede.

Por alguns segundos, toda vez que a câmera é ligada, o servidor web e o atualizador do
fabricante rodam antes que este firmware assuma o controle. O bloqueio isola essa
janela da Internet. A partir de agora, o aplicativo JOOAN não consegue mais acessar
esta câmera. Isso é esperado.

**Funcionou se** o aplicativo mostrar a câmera como offline e `ping CAMERA_IP`
continuar respondendo.

### 4. Prepare o PC

Baixe tudo em uma pasta nova:

1. O **código-fonte da v0.2.21**, de
   <https://github.com/ADCDS/jooan-w3u-local-firmware/archive/refs/tags/v0.2.21.zip>.
   Descompacte-o. Isso cria a pasta `jooan-w3u-local-firmware-0.2.21`.
2. Estes seis arquivos da
   [página da versão v0.2.21](https://github.com/ADCDS/jooan-w3u-local-firmware/releases/tag/v0.2.21).
   Coloque-os juntos em uma pasta chamada `release`, ao lado da pasta do código-fonte:
   `JOOAN_FW_PKG`, `JOOAN_FW_PKG.sha256`, `JOOAN_UNINSTALL`,
   `JOOAN_UNINSTALL.sha256`, `manifest.json` e `manifest.json.sig`.

O código-fonte precisa ser o da v0.2.21, não o do branch `main` atual. A ferramenta de
envio atual rejeita o pacote da v0.2.21 com `signed release target/ABI contract does
not match this repository`.

Instale o `cryptography`.

Linux ou macOS:

```sh
python3 -m pip install --user cryptography
```

No Debian ou no Ubuntu, `sudo apt install python3-cryptography` também funciona.

Windows (Prompt de Comando):

```bat
py -m pip install cryptography
```

**Funcionou se** este comando exibir um número de versão:

```sh
python3 -c "import cryptography; print(cryptography.__version__)"
```

No Windows, digite `py` no lugar de `python3`, aqui e no passo 5.

### Entrega via OTA: como a câmera realmente obtém o pacote

Leia isto antes do passo 5. O atualizador original **puxa** o pacote da nuvem, em vez
de receber um upload. `tools/upload_ota.py` o aciona, mas o que ele envia por POST é
ignorado: em seguida, a câmera baixa o firmware de uma URL fixa na nuvem,
`https://use1api.jooaniot.com/fw/JOOAN_FW_PKG`, via TLS. Portanto, uma instalação tem
dois requisitos além de executar a ferramenta:

1. **Para a câmera, `use1api.jooaniot.com` precisa resolver para o PC que vai
   servir o pacote.** É a parte que a maioria das pessoas deixa passar. A câmera **ignora
   o servidor DNS que o seu roteador distribui por DHCP** e consulta resolvedores públicos
   fixos no código (223.6.6.6 e 8.8.8.8). Mudar "o seu DNS" não adianta nada. O
   redirecionamento tem de acontecer no caminho da rede. Duas formas que funcionam:

   - **Um roteador que sobrescreva esse nome para a câmera.** No OpenWrt, basta uma
     linha do dnsmasq, `address=/use1api.jooaniot.com/<PC-IP>`, mais uma regra de firewall
     que **redirecione o tráfego da própria câmera na porta 53 para o resolvedor do
     roteador** (um DNAT das portas UDP/TCP 53 da câmera para o roteador), porque, caso
     contrário, a câmera contorna o DNS do roteador. Restrinja as duas ao MAC da câmera.
   - **Um dispositivo de redirecionamento de DNS na sub-rede da câmera** que responda a
     esse único nome com o endereço do PC e encaminhe todo o resto, também com a porta 53
     da câmera apontada para ele.

   Não é preciso simular o resto da nuvem. Só este nome de host, e só para o
   download do firmware.

2. **O PC precisa servir o pacote na porta TCP 443.** `tools/serve_ota.py` faz
   exatamente isso e nada mais: responde a `/fw/JOOAN_FW_PKG` com o arquivo que você
   indicar, usando um certificado autoassinado (a câmera não o valida), e
   devolve 404 para qualquer outro caminho. Ele não se faz passar pela API da nuvem, não
   repassa nada a servidores externos nem mexe nas credenciais do dispositivo.

Se a sua câmera já roda o jooan-local (uma atualização posterior, não a primeira instalação),
nada disso se aplica: as atualizações passam pela API autenticada
`/api/v1/update`, e não pelo download a partir da nuvem.

### 5. Instale

Primeiro, sirva o pacote. Na pasta `jooan-w3u-local-firmware-0.2.21`, em um terminal
(é preciso ter privilégios para escutar na porta 443; no Linux, use `sudo`):

```sh
sudo python3 tools/serve_ota.py --pkg ../release/JOOAN_FW_PKG
```

Ele exibe `[ready] https://0.0.0.0:443/fw/JOOAN_FW_PKG` e gera um certificado
descartável para `use1api.jooaniot.com`. Deixe-o rodando. Configure o
redirecionamento de DNS da seção anterior para que a câmera chegue a este servidor.

Depois, em um segundo terminal na mesma pasta, acione a câmera:

```sh
python3 tools/upload_ota.py --host CAMERA_IP --pkg ../release/JOOAN_FW_PKG
```

A ferramenta verifica a assinatura do pacote, aciona o atualizador do fabricante na
câmera (que baixa o pacote do servidor que você iniciou acima) e espera a câmera
reiniciar com o novo firmware. O terminal do servidor exibe
`[served] /fw/JOOAN_FW_PKG -> ... bytes` quando a câmera baixa o pacote. O processo leva de um
a cinco minutos. Mantenha a câmera ligada.

**Funcionou se** a última linha for:

```text
[+] jooan-local is active
```

Se falhar:

| Mensagem | Significado e o que fazer |
|---|---|
| `error: signed release target/ABI contract does not match this repository` | Você está usando outra versão do código-fonte. Use o zip da v0.2.21 do passo 4 |
| `... requires the Python 'cryptography' package` | Instale-o como no passo 4 |
| `[-] unexpected OEM response HTTP ...` | O atualizador do fabricante na câmera recusou o acionamento. Pare por aqui |
| O terminal do servidor nunca exibe `[served] ...` | A câmera não conseguiu acessar o seu servidor. A causa quase sempre é o redirecionamento de DNS: confirme que `use1api.jooaniot.com` resolve para o seu PC **do ponto de vista da câmera** e que a porta 53 da câmera está redirecionada (ela ignora o DNS do DHCP). Verifique se a porta 443 está aberta no PC |
| `[-] package upload completed but jooan-local did not become healthy` | A câmera recebeu o pacote, mas este firmware não iniciou. O mais provável é que o instalador tenha encontrado arquivos do fabricante diferentes e recusado a câmera antes de gravar qualquer coisa. A câmera mantém o firmware do fabricante. Pare por aqui e veja [Se algo der errado](#if-something-goes-wrong) |
| Nenhuma resposta | Confira o `CAMERA_IP` e se o PC e a câmera estão na mesma rede |

### 6. Faça login

Abra `http://CAMERA_IP/` em um navegador e faça login:

```text
username: admin
password: admin
```

A senha continua sendo `admin` até que você a altere na seção System (Sistema). A
mesma senha é usada no SSH e no RTSP, e alterá-la muda a senha dos três.

**Funcionou se** `http://CAMERA_IP/api/v1/setup/status` mostrar
`"release_version":"0.2.21"`.

<a id="7-back-up-the-camera-over-ssh"></a>
### 7. Faça backup da câmera por SSH

Recomendado. Isso salva no seu PC uma cópia de cada partição da flash, sem hardware
adicional. Só essa cópia pode recuperar esta câmera se uma atualização futura der
errado: os arquivos do fabricante que ela contém não são publicados em lugar nenhum.
Restaurar uma câmera que não inicializa mais também exige um gravador de flash (veja
[Recuperação](./RECOVERY.md#external-spi-recovery)).

A câmera tem sete partições, de `mtd0` a `mtd6`. Este comando as lista:

```sh
ssh admin@CAMERA_IP cat /proc/mtd
```

Em seguida, salve cada partição. Execute cada comando e digite a senha quando ela for solicitada.
No Windows, use o Prompt de Comando, não o PowerShell. O `>` do Windows PowerShell
corrompe arquivos binários.

```sh
ssh admin@CAMERA_IP "cat /dev/mtdblock0" > camera-mtd0.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock1" > camera-mtd1.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock2" > camera-mtd2.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock3" > camera-mtd3.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock4" > camera-mtd4.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock5" > camera-mtd5.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock6" > camera-mtd6.bin
```

**Funcionou se** os tamanhos dos sete arquivos somarem 8388608 bytes (8 MiB) e, para
`camera-mtd4.bin` (a partição de aplicação), a soma MD5 calculada na câmera:

```sh
ssh admin@CAMERA_IP md5sum /dev/mtdblock4
```

for igual à calculada no PC: `md5sum camera-mtd4.bin` (Linux),
`md5 camera-mtd4.bin` (macOS) ou `certutil -hashfile camera-mtd4.bin MD5`
(Windows).

Esses arquivos contêm a identidade desta câmera, a senha do Wi-Fi e as chaves dela.
Mantenha-os privados, nunca os compartilhe e nunca os grave em outra câmera.

<a id="8-use-the-camera"></a>
### 8. Use a câmera

Com a v0.2.21:

- **Vídeo:** RTSP na porta 554, usuário `admin` e a senha de administrador.
  `rtsp://CAMERA_IP:554/live/ch0` é a lente do cabeçote pan/tilt, e
  `rtsp://CAMERA_IP:554/live/ch1` é a lente fixa.
- **Pan/tilt e presets:** ONVIF em `CAMERA_IP`, porta 80, com o mesmo usuário e
  a mesma senha.
- **SSH:** `ssh admin@CAMERA_IP`, com a mesma senha.
- **Wi-Fi:** a câmera continua na rede que o aplicativo configurou. Para trocar de rede,
  informe a nova em Network → Wi-Fi (Rede → Wi-Fi) no console web, prepare-a
  (Stage) e confirme-a (Commit staged) assim que a câmera tiver se reconectado. Uma
  mudança que nunca é confirmada é revertida.
- **Relógio:** a câmera não tem relógio alimentado por bateria. Após uma queda de energia, clique em
  "Sync time & zone to this device" (sincronizar hora e fuso com este dispositivo) em
  System → Camera time (Sistema → Hora da câmera).

A documentação da v0.2.21 descreve esses recursos. Consulte-a na tag correspondente:
[README](https://github.com/ADCDS/jooan-w3u-local-firmware/blob/v0.2.21/README.md)
e [API](https://github.com/ADCDS/jooan-w3u-local-firmware/blob/v0.2.21/docs/API.md).
Os documentos no `main` descrevem a 0.2.39.

<a id="updating-later"></a>
## Atualizações posteriores

Instale uma versão publicada mais recente pelo console web. Na seção Firmware,
escolha o `JOOAN_FW_PKG` dessa versão, espere aparecer "signature verified"
(assinatura verificada) e então aplique-a. A câmera reinicia e fica offline por
cerca de 90 segundos. Uma versão só é aceita se for assinada por este projeto e
for mais recente que a instalada.

**Não atualize uma câmera com a v0.2.21 para a 0.2.22 ou posterior, a menos que vá
instalar a camada de mídia logo em seguida.** A partir da 0.2.22, a plataforma não
fornece mais o vídeo por conta própria, então, até lá, a câmera fica sem vídeo.

As versões posteriores à 0.2.21 só respondem quando a câmera é acessada por um endereço
IP privado ou pelo próprio nome dela. O nome é `jooan-w3u`, a menos que você o mude
em Network → Local name (Rede → Nome local). Ele pode vir sozinho ou terminar em `.local`,
`.lan`, `.home.arpa` ou `.internal`, como em `jooan-w3u.local` ou
`jooan-w3u.lan`. Qualquer outro nome recebe `403 host_rejected` (veja
[Segurança de rede](../../../docs/NETWORK-SECURITY.md#host-and-origin-checks)).

## Desinstalação

Na seção Firmware, aplique o arquivo `JOOAN_UNINSTALL` da versão que você
instalou. Após a reinicialização, a câmera volta a rodar o firmware do fabricante sem
confinamento: o servidor web do fabricante volta, e a câmera pode acessar a nuvem
de novo, a menos que o roteador ainda a bloqueie. A câmera mantém as configurações de
Wi-Fi do próprio fabricante. As configurações da plataforma, inclusive uma rede Wi-Fi
confirmada nela, são apagadas. Os detalhes estão em
[Recuperação](./RECOVERY.md#booting-camera-rollback).

**Com a camada de mídia instalada, a desinstalação não a remove.** Os arquivos dela em
`/opt/jooanipc-open` permanecem, inclusive a senha de stream e a chave privada TLS,
então apague-os antes, por SSH: `rm -rf /opt/jooanipc-open`. A partição de aplicação
também continua sendo a reduzida, sem as mensagens de voz do fabricante e os drivers
de outros sensores. **Não tente regravá-la com a câmera em funcionamento:** o programa
que roda a partir dessa partição morre quando ela é apagada e, se a câmera reiniciar antes
do fim da gravação, só um gravador de flash poderá recuperá-la. Para restaurar a original, são necessários
um gravador de flash e o `my-appfs.img` que você salvou.

<a id="if-something-goes-wrong"></a>
## Se algo der errado

- **A câmera inicializa com o firmware do fabricante após o passo 5.** Se o instalador
  recusou a câmera, ele não gravou nada e a câmera está inalterada. Você pode voltar
  a usar o aplicativo JOOAN removendo o bloqueio no roteador.
- **O console web ou o SSH param de responder após uma atualização.** Desligue e
  religue a câmera uma vez e espere três minutos. Se ainda assim não responder, siga
  [Recuperação](./RECOVERY.md).
- **A câmera não inicializa mais de jeito nenhum.** Só uma restauração do seu backup
  no nível do chip, com um gravador de flash, a traz de volta
  ([Recuperação](./RECOVERY.md#external-spi-recovery)).

Ao pedir ajuda, nunca publique imagens da flash, senhas nem chaves (veja a
[Política de segurança](../../../SECURITY.md#reporting-a-vulnerability)).

<a id="the-media-layer"></a>
## A camada de mídia

A partir da 0.2.22, vídeo, ONVIF e pan/tilt vêm do daemon de mídia aberto
[jooanipc-open](../../../media/), e não do programa do fabricante. Em uma inicialização
nativa, o padrão desde a 0.2.23, o programa do fabricante nunca é iniciado. Essa é a
configuração que o restante deste repositório descreve.

**Todo esse processo já foi executado uma vez, de ponta a ponta, em uma câmera real.** Uma câmera de
teste foi restaurada ao firmware original genuíno, configurada no Wi-Fi sem o aplicativo do
fabricante, atualizada para a plataforma via OTA e, por fim, teve gravada na flash uma imagem
de mídia gerada a partir da sua própria partição. Ela transmitiu vídeo por RTSP e, após um
desligamento completo da energia, voltou rodando a pilha aberta. O que o teste verificou, e onde a evidência ainda é escassa:

| Etapa | Verificado |
|---|---|
| Retorno ao firmware original | A câmera de teste foi regravada com a partição original (com o watchdog alimentado e reversão em RAM) e inicializou o firmware original genuíno, ainda sem provisionamento |
| Provisionamento sem aplicativo | A partir de um PC próximo, um `GET /goform/setupWifiInfo` no SoftAP da câmera a colocou no Wi-Fi; a câmera informou sucesso e se conectou |
| Instalação da plataforma via OTA | Com a nuvem redirecionada para um servidor local, a câmera baixou e aplicou o pacote assinado da plataforma e inicializou com ele |
| A imagem | Gerada a partir da partição original real; os mesmos arquivos, byte a byte, de uma imagem de mídia comprovadamente funcional |
| A configuração | O que o instalador grava passa na verificação do próprio daemon distribuído |
| O instalador | Suas decisões, sua sequência e suas verificações prévias, tanto em um substituto simulado quanto na câmera real; ele recusou corretamente até que a plataforma e o Wi-Fi estivessem prontos |
| O script de gravação, `first-appfs.sh` | Rodou de verdade: gravou a imagem de mídia, manteve a reversão em RAM, e o daemon aberto qualificou a inicialização nativa e manteve o controle do watchdog |
| Sobrevive à reinicialização | Após desligar e religar a energia por completo, a câmera voltou com a imagem de mídia aberta e transmitiu RTSP, sem reverter para o firmware original |
| Instalação da 0.2.39 a partir do firmware original | Feita pelo mesmo atualizador durante esse teste |

Dois problemas menores encontrados nesse teste já foram corrigidos:

- Uma rede Wi-Fi **aberta** (sem senha) agora pode ser confirmada pela
  plataforma (a conexão usa `key_mgmt NONE`); redes WPA2 funcionam como antes.
- Uma plataforma recém-instalada pode deixar o `/opt` de 384 KiB com espaço por pouco
  insuficiente para a primeira instalação de mídia. O instalador agora libera automaticamente o
  `/opt/conf/config.org` original, que pode ser regenerado, quando falta espaço e a cópia
  original está disponível para restaurá-lo (a inicialização original o recria de `/mnt/mtd/etc`).

Se a gravação falhar na verificação por releitura, o script restaura a partição a partir
de uma cópia em RAM. **Se faltar energia durante a gravação, só um gravador de flash e um
backup dessa partição podem trazer a câmera de volta** (veja
[Recuperação](./RECOVERY.md#appfs-reflash-recovery)). A gravação já foi concluída em
uma câmera real, mas só uma vez; antes, faça um backup completo de todas as partições
(o instalador faz isso) e mantenha a câmera com alimentação estável durante os poucos
minutos que o processo leva.

Você precisa de um computador com Linux ou WSL, com `squashfs-tools`, Python 3, OpenSSH
e OpenSSL, e dos arquivos da v0.2.39 da página da versão.

1. **Instale a plataforma.** Siga os passos 4 e 5 com o código-fonte e os arquivos da
   v0.2.39 ou, a partir da v0.2.21, atualize como em [Atualizações posteriores](#updating-later).
   A partir daqui, a câmera fica sem vídeo até o último passo. O console web continua em
   `http://CAMERA_IP/`.
2. **Faça backup da câmera** ([passo 7](#7-back-up-the-camera-over-ssh)).
3. **Confirme a rede Wi-Fi da câmera** em Network → Wi-Fi (Rede → Wi-Fi) no console web,
   mesmo que seja a rede em que a câmera já está. Uma inicialização nativa usa somente a
   rede confirmada ali, e não a que o aplicativo JOOAN configurou.
4. **Descompacte** `jooanipc-open-*.tar.gz` e `libimp-*.tar.gz` lado a lado
   e abra um terminal na pasta `jooanipc-open-*`.
5. **Copie a partição de aplicação da sua câmera e gere a sua imagem a partir dela.**
   Ambas contêm arquivos do fabricante, então mantenha-as privadas:

   ```sh
   ssh admin@CAMERA_IP 'cat /dev/mtdblock4' > my-appfs.img
   sh media/tools/build-appfs.sh --first my-appfs.img bin/jooanipc-open ../libimp-*/libimp.so bin/libatomic.so.1 my-bundle
   ```

   O script recusa uma partição em que falte um arquivo de que ele precisa ou que contenha
   um programa do fabricante diferente daquele fixado pela plataforma.

6. **Verifique a câmera.** Isto não altera nada:

   ```sh
   CAMERA_PASSWORD=admin python3 media/scripts/install-camera.py my-bundle --camera CAMERA_IP
   ```

7. **Instale,** escolhendo a senha dos streams de vídeo da câmera:

   ```sh
   CAMERA_PASSWORD=admin STREAM_PASSWORD=your-choice python3 media/scripts/install-camera.py my-bundle --camera CAMERA_IP --yes-untested
   ```

   Mantenha a câmera ligada. **Funcionou se** a última linha disser que a versão está
   em execução. A interface de administração agora fica em `https://CAMERA_IP/`, com o
   certificado da própria câmera, então o navegador exibe um aviso uma vez. Faça login com a
   senha de administrador. `http://CAMERA_IP/` agora só redireciona para lá.

A inicialização se autoverifica: uma inicialização nativa que registra uma falha, ou que
é interrompida três vezes seguidas, faz a próxima recorrer ao programa do fabricante.
O mesmo vale para uma sem gateway após 5 minutos, até que uma inicialização nativa
tenha se qualificado uma vez (veja [Recuperação](./RECOVERY.md#a-media-change-that-stops-native-boot)).

**No Windows,** gere o pacote de versão no WSL e depois execute o instalador com `py` no
Prompt de Comando, usando `set` para cada senha. Ele usa o OpenSSH que vem com o
Windows e precisa do `openssl` no PATH, que o Git for Windows inclui.
O OpenSSH 8.4 ou posterior (Windows 11) lê a senha de `CAMERA_PASSWORD`;
a versão do Windows 10 pede a senha a cada conexão, então adicione antes uma chave SSH
no console web. Isso também ainda não foi executado no Windows.

**As atualizações de mídia posteriores** partem da partição atual da sua câmera, usando o
`build-appfs.sh` sem `--first` e um `bundle.json` que você escreve para cada uma (veja
[pacotes de versão da câmera](../../../media/docs/hardware.md#camera-release-bundles)). Nesse
caso, o instalador também precisa de `MEDIA_PASSWORD`, a senha de stream, para verificar o
resultado. O formulário Sistema → Mídia da interface de administração só aceita pacotes
assinados com a chave de assinatura de versões deste projeto. Toda atualização de mídia,
seja pelo instalador, seja por esse formulário, regrava a mesma partição que a primeira
instalação, então o aviso acima sobre queda de energia também vale, inclusive para resets
da própria placa. Como uma imagem contém arquivos do fabricante, nenhuma é publicada;
por isso, esse formulário serve a quem detém a chave ou gera a plataforma com chave própria.

**Cartão SD:** no modo de inicialização de reserva, a camada de mídia pode executar o
daemon a partir de um cartão microSD em vez da partição de aplicação. Só o programa do
fabricante monta o cartão, então isso só funciona nesse modo de reserva. Não é uma forma
de contornar nenhum dos passos acima.

O formato de um pacote de versão está descrito em
[pacotes de versão da câmera](../../../media/docs/hardware.md#camera-release-bundles).
