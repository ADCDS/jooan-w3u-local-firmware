<!-- Translated from docs/RECOVERY.md. Authoritative version: ../../RECOVERY.md -->

# Recuperação

## Níveis de recuperação

São necessários dois caminhos independentes:

- **Reversão/desinstalação por pacote** para uma câmera que ainda inicializa e cujo
  gerenciamento local ou canal de atualização do fabricante ainda funciona.
- **Recuperação externa via SPI** para uma câmera que não inicializa mais ou está inacessível.

A regravação da appfs feita pela camada de mídia acrescenta um terceiro caminho, manual, para
o único caso em que ela mantém a câmera funcionando com uma partição danificada; veja
[Recuperação de falhas na regravação da appfs](#appfs-reflash-recovery).

Um pacote de desinstalação guardado apenas na câmera não é um método de recuperação
independente. Guarde-o fora do dispositivo. A restauração externa via SPI continua sendo o
caminho mais robusto para uma câmera que não consegue inicializar nem aceitar uma atualização.

## Material de recuperação

Guarde estes itens offline e identifique-os como pertencentes a uma única câmera física:

- duas leituras completas da flash de 8 MiB, idênticas byte a byte, e seus hashes SHA-256,
  feitas por SSH depois que a plataforma estiver rodando
  ([Instalação, passo 7](./INSTALL.md#7-back-up-the-camera-over-ssh)) ou com
  um gravador de flash SPI;
- fotografias da PCB `JA-6621 V1.0`, da orientação do chip de flash, da orientação da garra
  e da fiação;
- o modelo do gravador, a tensão configurada, o modelo de chip de flash selecionado e um
  comando de leitura que comprovadamente funciona;
- os pacotes exatos de instalação e desinstalação, com os manifestos e os hashes;
- uma leitura da partição appfs (`/dev/mtd4`) feita antes de cada atualização da camada de
  mídia; o diretório `backups/` do pacote de versão guarda apenas arquivos de configuração;
- uma cópia do código-fonte da versão e dos metadados da toolchain;
- anotações, sem dados sensíveis, que descrevam a última inicialização bem-sucedida e o
  estado da rede.

Nunca compartilhe o dump de uma câmera como imagem de recuperação para outra unidade.

<a id="booting-camera-rollback"></a>
## Reversão em uma câmera que ainda inicializa

1. Leve a câmera e a estação de trabalho para a rede de recuperação isolada.
2. Bloqueie no roteador todo o acesso da câmera à Internet.
3. Verifique o hash do pacote de desinstalação e os metadados de alvo `A12`.
4. Use o caminho de atualização local autenticado ou o canal offline documentado.
5. Mantenha a alimentação estável até que o pacote informe a conclusão e a reinicialização termine.
6. Faça uma inicialização a frio, inspecione o status e os sockets de escuta e confirme o estado restaurado pretendido.

A desinstalação preserva os dados de fábrica e a configuração de Wi-Fi armazenada pelo
firmware do fabricante, mas apaga o estado da plataforma, inclusive a rede Wi-Fi confirmada
(`wifi.json`); depois disso, o programa do fabricante usa a rede que ele próprio tem
armazenada, que pode não ser a que a câmera estava usando. Se a câmera não conseguir
autenticar, não conseguir validar o pacote ou reiniciar repetidamente, pare de tentar
atualizações pela rede e use a recuperação externa.

A substituição do runtime é uma transação de manutenção: o controlador assinado e o pacote de
recuperação SSH continuam disponíveis enquanto o único runtime persistente é substituído.
Durante o teste de integridade de dois minutos, o supervisor aplica a rede Wi-Fi confirmada
(uma inicialização nativa sem nada pelo que esperar desiste do gateway após 300 s) e reinicia um
candidato que falhou ao iniciar: o `start.sh` tem limite de 15 s, enquanto o `entropy-ready.sh` espera até
30 s, e, no início de uma inicialização nativa, a entropia é coletada lentamente. Se o candidato
ainda assim falhar, ou se o teste for interrompido, ele continua selecionado como o único runtime,
porque a atualização já apagou o anterior, e o supervisor continua a reiniciá-lo; recupere a
câmera pelo SSH do próprio controlador e tente de novo ou desinstale.
(Antes de isso ser corrigido, um teste malsucedido removia o candidato e deixava a câmera sem
runtime; o parágrafo abaixo descreve esse estado.) O runtime antigo pode continuar rodando a partir do tmpfs até a reinicialização feita pela atualização, mas não é um
slot de reversão gravado na flash. Uma atualização da plataforma que falha não recorre ao
atualizador do fabricante: o GoAhead continua sendo encerrado enquanto o controlador estiver em execução.

Sem runtime, nada aplica a rede Wi-Fi confirmada. Uma inicialização nativa, então, não
consegue alcançar o gateway, percebe que nada está tentando alcançá-lo, falha após 300 s e grava o marcador persistente `native-failed`,
e as inicializações seguintes executam o programa do fabricante sob contenção, que se conecta
à rede que ele próprio tem armazenada. Na unidade verificada, essa é a rede de testes de bancada
do fabricante, `JOOANTEST` (aberta e sondada como SSID oculto). Para acessar a câmera nesse estado:

1. Crie uma rede `JOOANTEST` temporária, oculta e aberta, em um ponto de acesso ao alcance
   da câmera, restrita ao MAC da câmera e em uma sub-rede isolada.
   A câmera se conecta em poucos segundos. Ela pode não obter um endereço IPv4, mas o seu
   endereço IPv6 link-local responde ao SSH (o dropbear escuta em `[::]:22`), por
   exemplo, por meio de `ssh -L 2222:[fe80::…%<bridge>]:22` nesse ponto de acesso.
2. Instale o runtime como o slot estável: copie `runtime.tar.gz` e
   `runtime.md5` da área de preparação da versão (release stage) para `/opt/custom/jooan-local/slots/A/`
   e então, com o `common.sh` carregado no shell, execute `jl_lock; jl_write_selection A - 0; jl_unlock`.
3. Apague `/opt/jooanipc-open/native-failed`, execute `sync` e reinicie. O supervisor
   inicia o runtime (e o reinicia se ele falhar ao iniciar pela primeira vez), o hook do
   runtime aplica a rede confirmada e a inicialização nativa se qualifica.
4. Remova a rede temporária.

## Uma atualização parou

Em uma câmera com pouco espaço livre, uma atualização da plataforma apaga o único runtime da flash antes
de gravar o novo. Se ela parar no meio do caminho, a câmera continua funcionando a partir da RAM, sem nada
na flash para iniciar da próxima vez: a página Sistema informa que a atualização parou, e um reinício é
recusado com `no-runtime`.

**Não reinicie nem desconecte a câmera da tomada ainda.** A próxima inicialização ficaria sem rede, e você
teria de recorrer aos passos acima. Por SSH:

```sh
cat /opt/custom/jooan-local/state/selection
ls /opt/custom/jooan-local/slots/
```

Se a seleção indicar um slot (`A - 0`), esse slot existir e `df -k /opt` mostrar pelo menos 51 KiB
livres, nada foi perdido. Com menos de 50 KiB livres na inicialização, o `startapp` do fabricante apaga `/opt/etc` e
`/opt/jooanipc-open`, o que é pior do que ficar sem runtime; por isso, libere espaço primeiro. O que pode ser apagado:
qualquer coisa com nome `*.new`, `*.next` ou `*.previous` em `/opt/custom/jooan-local` e `/opt/jooanipc-open`, além de
`state/net-loss.log`. O que não pode: `config/`, `state/selection`, `controller.tar.gz`, `recovery/`, um slot
selecionado e, em `/opt/jooanipc-open`, `camera.ini`, `deployment.env`, os dois scripts de inicialização e as chaves. Se a
seleção mostrar `- - 0`, coloque um runtime de volta. Antes, interrompa qualquer instalador que ainda esteja esperando
por esta câmera: ele seguiria em frente ao ver um reinício.

1. Localize o pacote que falhou, que permanece na RAM até um reinício:
   ```sh
   ls /run/jooan-local/firmware.*.sqfs
   mkdir /tmp/fw && mount -t squashfs -o ro,loop /run/jooan-local/firmware.<id>.sqfs /tmp/fw
   ```
   Se ele não estiver mais lá, os mesmos dois arquivos estão no `JOOAN_FW_PKG` da versão. No seu computador,
   execute `dd if=JOOAN_FW_PKG bs=96 skip=1 of=p.sqfs && unsquashfs p.sqfs runtime.tar.gz runtime.md5`; depois, execute `mkdir -p /tmp/fw`
   na câmera e copie os arquivos para lá, com `ssh admin@CAMERA 'cat > /tmp/fw/runtime.tar.gz' < squashfs-root/runtime.tar.gz`.
2. Copie o runtime para um slot e deixe que a soma de verificação decida. Antes, confira se `df -k /opt` mostra pelo
   menos o tamanho de `runtime.tar.gz` mais 60 KiB; se não mostrar, libere espaço, pelo motivo explicado acima.
   ```sh
   cd /opt/custom/jooan-local/slots
   rm -rf A.new
   mkdir A.new && cp /tmp/fw/runtime.tar.gz /tmp/fw/runtime.md5 A.new/
   [ "$(md5sum A.new/runtime.tar.gz | cut -c1-32)" = "$(cat A.new/runtime.md5)" ] && sync && mv A.new A && sync
   ```
3. Selecione-o com as próprias funções do controlador em execução:
   ```sh
   . /run/jooan-local/controller/boot/common.sh; jl_lock; jl_write_selection A - 0; jl_unlock
   ```

O supervisor só lê a seleção na inicialização, então isso não interfere em nada. Com `df -k /opt` em 51 KiB ou
mais, a câmera já pode reiniciar com segurança, e a atualização pode ser tentada de novo. A página de administração
mantém Reiniciar e Restaurar bloqueados até que a câmera tenha reiniciado; por isso, reinicie-a na mesma sessão SSH
com `jl_reboot_asked`, que primeiro salva o relógio e informa à próxima inicialização que a reinicialização foi
solicitada. Isso foi feito de verdade na camera1 em 2026-11-07, a partir da RAM.

<a id="a-media-change-that-stops-native-boot"></a>
## Uma mudança na camada de mídia que impede a inicialização nativa

Se o daemon de mídia se encerrar durante uma inicialização nativa, o watchdog que ele armou reinicia a placa.
Uma inicialização que registrou a falha faz com que a seguinte grave o marcador persistente `native-failed` e
passe para o modo de transferência (handoff); uma inicialização apenas interrompida, por uma queda de energia ou
um travamento, ainda ganha mais duas tentativas antes disso. Uma transferência que também falha grava o marcador
persistente `boot-trial` e deixa o programa do fabricante rodando sob contenção. Uma reinicialização solicitada
pela própria plataforma não conta contra a inicialização que ela interrompeu, mas não anula uma falha registrada
antes dela.

Uma inicialização nativa sem gateway espera, sem reset e sem marcador, enquanto houver chance de a espera
dar resultado: uma rede confirmada, um runtime tentando se conectar a ela e uma inicialização anterior que
já se qualificou nesta câmera. Caso contrário, ela falha após 300 s, como descrito acima. Uma versão que perde
o gateway depois de iniciar ainda assim se qualifica, a menos que isso aconteça em três inicializações seguidas.
Uma rede confirmada que deixou de existir de vez, em uma câmera que já se qualificou antes, deixa a câmera
esperando: para acessá-la, volte a oferecer uma rede com esse nome e essa senha. A
plataforma e o SSH continuam respondendo, mas a interface HTTPS não. Para recuperar:

1. Leia o motivo da falha em `/tmp/jooanipc-open.log` ou, se a placa tiver reiniciado desde então, em um
   log espelhado para fora da câmera. Procure com grep por `startup failed` e `health check` e, nas
   mensagens do kernel, por `Process `: um oops significa que a causa pode não ser a versão.
2. Desfaça a causa, se ela for uma configuração salva. Por exemplo, em
   `/opt/jooanipc-open/state/controls.json` ou `/opt/jooanipc-open/camera.ini`.
3. Execute `cd /opt/jooanipc-open && rm -f native-failed native-trial native-retry boot-trial && sync`
   e reinicie. A inicialização volta a ser nativa, e o instalador aceita a câmera.

Isso aconteceu com a câmera de teste em 2026-11-06, duas vezes, com uma versão que não conseguia
iniciar com o modo privacidade ligado. Nas duas vezes, ela foi recuperada dessa forma em cerca de 5 minutos.

Aconteceu uma terceira vez naquele dia, com uma versão sem defeitos: `IMP_ISP_Open failed (-1)`. Um
script de monitoramento leu `/proc/jz/isp/isp-m0` por SSH enquanto a placa inicializava. Se esse arquivo
for lido depois que o módulo do ISP é carregado e antes que o daemon abra o ISP, ele
provoca um oops no driver (`Process awk` nas mensagens do kernel); foi o que aconteceu, e a abertura do ISP
pelo daemon, um segundo depois, falhou. Uma inicialização nativa fica nesse estado por 30 s ou mais enquanto espera
o Wi-Fi. Qualquer coisa que leia esse arquivo periodicamente precisa primeiro ver `"ready":true` em
`/api/v1/status`.

A desinstalação assinada é uma remoção destrutiva, não a restauração do estado anterior.
Ela retira o console web, a remoção das rotas padrão, o mecanismo de contenção do processo, o SSH do
projeto e o controlador/runtime compactados; a próxima inicialização volta ao comportamento do
fabricante/GoAhead, com o programa do fabricante rodando sem confinamento. Os arquivos da camada de
mídia em `/opt/jooanipc-open` e a versão dela na appfs permanecem na flash, mas nada os inicia
depois que o `local.rc` deixa de existir. A desinstalação não restaura o antigo hook de telnet sem
autenticação nem um `/opt/etc/local.rc` anterior que tenha sido arquivado.
Mantenha o isolamento no roteador ativo antes, durante e depois da desinstalação.

<a id="appfs-reflash-recovery"></a>
## Recuperação de falhas na regravação da appfs

`media/scripts/upgrade-appfs.sh` regrava `/dev/mtd4`, a appfs, que contém o `startapp` do
fabricante, o `jooanipc`, os módulos do kernel, o ajuste de imagem (IQ) dos sensores e a versão de mídia.
Antes de parar o daemon de mídia, o script obtém a trava de manutenção da plataforma
(`/run/jooan-local/state.lock`), que suspende os reinícios do runtime feitos pelo supervisor e a
reinicialização acionada pelo watchdog de rede, e um alimentador `watchdog` do rootfs assume o
watchdog de hardware já armado. Sistema → Mídia, na interface de administração, executa este mesmo script.
`media/scripts/first-appfs.sh`, usado na primeira versão de mídia de uma câmera, segue os
mesmos passos partindo do programa original do fabricante. A reversão dele também remove a configuração de
mídia, de modo que a câmera volta ao estado original, apenas com a plataforma.

- **Antes do ponto de confirmação** (a troca de configuração logo antes do
  `flashcp`), qualquer falha reinicia a câmera com a appfs intacta.
- **Depois dele**, uma gravação que falha na verificação é revertida a partir de uma cópia em RAM
  da appfs original, em até três tentativas; em seguida, os arquivos de configuração e o
  `native-boot.sh` são restaurados e a câmera reinicia.
- **Se a reversão também falhar**, o instalador não reinicia a câmera. Ela continua ligada,
  com o alimentador do rootfs rodando, a trava mantida e as duas imagens em
  `/tmp`: `/tmp/jipc-appfs-rollback.bin` (a appfs original) e
  `/tmp/jipc-flash-candidate.squashfs` (a nova imagem). O SSH continua funcionando,
  porque a plataforma roda a partir de `/run` e do rootfs.

Recupere a câmera desse estado por SSH, sem reiniciar:

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

Uma primeira instalação não guarda arquivos `.before`, porque não havia camada de mídia. Depois do
`flashcp` e da verificação, execute, em vez das três linhas `cp`:

```sh
cd /opt/jooanipc-open && rm -f boot-handoff.sh camera.ini deployment.env enabled native
```

O MD5 da appfs original é o segundo argumento passado ao instalador: o
`expected_current_appfs_md5` do pacote de versão, que o `install-camera.py` também exibe na
verificação prévia. Repita o `flashcp` até que o `md5sum` coincida; não reinicie antes
disso. `/tmp` fica na RAM: as imagens se perdem em qualquer reinicialização.

Se faltar energia, ou se a câmera reiniciar, enquanto a appfs estiver danificada, o `startapp`
estará ausente: nada carrega o `local.rc`, então nem a plataforma nem o SSH
iniciam. A recuperação passa então a exigir um gravador de flash SPI (SPI NOR de 8 MiB) e um
backup da partição appfs ou da flash inteira, como descrito abaixo.

<a id="external-spi-recovery"></a>
## Recuperação externa via SPI

A placa verificada usa uma flash SPI NOR de 8 MiB (Puya P25Q64HA na unidade observada).
Confirme qual chip está de fato montado na placa, em vez de presumir.

1. Desconecte a alimentação e o cabo Ethernet da câmera.
2. Use um gravador que opere com segurança em 3,3 V e confirme o pino 1 e a orientação pela
   placa e pelo datasheet da flash.
3. Leia o chip novamente antes de gravar e preserve essa imagem do estado de falha.
4. Confirme que o backup tem exatamente 8 MiB e que o SHA-256 dele corresponde ao de uma das
   leituras feitas antes da instalação.
5. Grave a imagem completa usando o comando verificado do gravador.
6. Leia o chip de volta e compare cada byte com o backup.
7. Desconecte o gravador antes de ligar a alimentação normal da câmera.
8. Inicialize a câmera somente na rede de recuperação isolada e verifique o kernel do fabricante,
   os dois sensores, o rádio e o comportamento de rede.

Os comandos específicos de cada gravador não foram generalizados aqui de propósito: a fiação da
garra, a tensão do controlador e o suporte a chips de flash variam. Siga a documentação do gravador e
do fabricante da flash. Nunca aplique 5 V a uma SPI NOR de 3,3 V.

## Depois da recuperação

Parta do princípio de que qualquer credencial ou chave privada presente durante uma falha de
segurança pode estar comprometida. Troque a senha de administrador, substitua as chaves SSH
cadastradas e substitua as credenciais de Wi-Fi. Valide de novo o isolamento no roteador antes de
devolver a câmera ao uso normal.

Registre por que a recuperação foi necessária e guarde os logs, com os dados sensíveis removidos. Não envie
imagens brutas da flash, chaves, tokens, cookies nem configurações de Wi-Fi para uma issue.

## Um endereço fixo na rede errada

Um endereço fixo se mantém após um reinício, e um roteador novo pode estar em outra sub-rede. A função
Restaurar padrões devolve o endereço ao modo automático e mantém a rede Wi-Fi; por isso, manter o botão de reset
pressionado por 5 segundos é o caminho de volta. Sem acesso ao botão: use um roteador temporário na
sub-rede antiga, com o endereço do gateway antigo, porque o daemon de mídia só inicia depois que esse
endereço responde. Em seguida, use a interface de administração, em Rede.

## A recuperação só está concluída quando

- o dispositivo inicializa a frio repetidas vezes;
- os dois caminhos CV2005 produzem o vídeo esperado;
- a interface SKW6316 está estável;
- o caminho de gerenciamento pretendido funciona;
- portas inesperadas e tráfego de saída inesperado estão ausentes ou bloqueados;
- a nova leitura completa da flash está arquivada com o seu hash.
