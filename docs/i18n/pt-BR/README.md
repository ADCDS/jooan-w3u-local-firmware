<!-- Translated from README.md. Authoritative version: ../../../README.md -->

# Firmware local para a JOOAN W3-U

<p align="center">
  <img src="../../../docs/images/w3u-camera.jpg" width="320"
       alt="A câmera JOOAN W3-U na mão de uma pessoa: duas lentes empilhadas em uma única carcaça pan/tilt branca, duas antenas Wi-Fi e um cabo rabicho Ethernet">
  <br>
  <sub>A câmera-alvo deste projeto: JOOAN W3-U (<code>JA-A12</code>), com lente dupla.</sub>
</p>

Firmware que prioriza o uso local, feito para uma única câmera específica, a
JOOAN W3-U (`JA-A12`). Ele desconecta a câmera da nuvem e dos serviços P2P da
JOOAN e os substitui por serviços locais e autenticados: um console web, SSH,
vídeo RTSP, além de pan/tilt e presets via ONVIF para um NVR como o Frigate.

Este não é um firmware de código aberto completo. O bootloader do fabricante, o
kernel Linux 3.10, os módulos do kernel, o ajuste dos sensores e o firmware do
rádio permanecem, assim como o programa de câmera do fabricante, `jooanipc`,
mantido como reserva. Duas camadas são instaladas por cima:

- a **plataforma** (a raiz deste repositório) é instalada na partição gravável
  `/opt`. Ela fornece o console web e a API, SSH, Wi-Fi, relógio e atualizações
  assinadas, e mantém a câmera fora da Internet;
- o **daemon de mídia** [jooanipc-open](../../../media/) é instalado na partição
  de aplicação. Ele disponibiliza vídeo, ONVIF e pan/tilt, de modo que, em uma
  inicialização normal (nativa), o programa do fabricante nunca chega a ser iniciado.

[Arquitetura](../../../docs/ARCHITECTURE.md) explica os modos de inicialização e
a fronteira de confiança.

<p align="center">
  <img src="../../../docs/images/webui-overview.png" width="480"
       alt="A página Resumo do console web local: as imagens das duas lentes (aqui desfocadas) e ações rápidas para visão noturna, luz branca, modo privacidade, fala e sirene">
  <br>
  <sub>O console web local (imagens das duas lentes desfocadas). Ele roda na câmera,
  dispensa a nuvem e está disponível em sete idiomas. Mais telas abaixo.</sub>
</p>

## Situação atual

- **Pré-lançamento de engenharia; nada tem suporte ainda.** As versões são testadas
  em uma única câmera física. Uma versão só passa a ter suporte quando as verificações
  de instalação e desinstalação assinadas, migração, Wi-Fi, hora de rede, SSH,
  roteamento, queda de energia e recuperação a frio forem aprovadas nessa câmera
  ([critérios de promoção em hardware](../../../tests/TEST_PLAN.md#hardware-promotion-gates)).
- **Publicada: v0.2.39** (2026-11-07): o pacote da plataforma e, pela primeira vez,
  os binários e scripts da camada de mídia. Cada proprietário ainda gera a imagem
  para a câmera a partir da partição de aplicação dessa mesma câmera, porque ela
  contém arquivos do fabricante; por isso, nenhuma imagem é publicada.
- **O processo completo já foi executado uma vez, de ponta a ponta, em uma câmera real.**
  Uma câmera de teste foi restaurada ao firmware original, configurada no Wi-Fi sem o
  aplicativo do fabricante, atualizada para a plataforma via OTA e teve gravada na flash
  uma imagem de mídia gerada a partir da sua própria partição; ela transmitiu vídeo por
  RTSP e continuou rodando a pilha de software aberta após uma reinicialização a frio (veja
  [o relatório do ciclo completo](../../../docs/reports/hardware-roundtrip-20261108.md) e
  [A camada de mídia](./INSTALL.md#the-media-layer)). Isso só aconteceu uma vez, então
  trate como pré-lançamento: faça o backup completo que o instalador gera e mantenha a
  câmera ligada durante a gravação.
- **v0.2.21** (2026-10-16, apenas a plataforma) continua publicada. É a última versão
  instalada em uma câmera com firmware original. Com ela, o vídeo ainda vem do
  programa do fabricante, executado sob confinamento.

## Escopo exato de hardware

Use este projeto somente com uma unidade que atenda a todos os itens a seguir:

- produto/modelo informado pelo firmware OEM: `JA-A12` / token de pacote `A12`;
- PCB: `JA-6621 V1.0`;
- SoC: Ingenic T23N;
- sensores: dois, `cv2005` e `cv2005s1`;
- rádio: USB SeekWave SKW6316 (`SV6160LITE` na unidade verificada);
- flash: SPI NOR de 8 MiB (unidade verificada: Puya P25Q64HA).

A embalagem de varejo da unidade verificada indica: marca `JOOAN`, modelo `W3-U`,
FCC ID `2BBQ4-W3U`, 5 V / 1,5 A. Isso identifica apenas o nome do produto, o que
por si só não basta: quem define a compatibilidade é a lista de verificação acima.
A imagem do Thingino chamada "Jooan W3-U" se destina a outra revisão, com
SC2336P/ATBM6132U, e não é intercambiável. Nome do produto e formato da carcaça não
são evidência de compatibilidade. Leia [Compatibilidade](../../../docs/COMPATIBILITY.md)
antes de instalar qualquer coisa.

## O que funciona e o que não funciona

Com a versão 0.2.39 e sua camada de mídia:

- **Funciona:** RTSP para as duas lentes (1920×1080 e 1024×576 a 15 fps, H.264); pan/tilt
  ONVIF e 16 presets com nome para o Frigate; retorno à posição inicial a cada inicialização;
  inversão de imagem, modo noturno/IR-cut manual e luz branca; uma única interface de administração
  em `https://<camera>/` para celulares e computadores (`http://<camera>/` redireciona para lá); SSH;
  Wi-Fi com lista de redes, endereço estático, preparação, confirmação e reversão; reinício,
  reinício diário e restauração dos padrões; modo privacidade; patrulha por presets; movimentos
  ONVIF exatos; hora de rede opcional a partir de um servidor local; mDNS; atualizações assinadas.
- **Ligado, exceto com `audio=0`:** áudio (microfone no RTSP, fala pelo alto-falante a partir
  do Frigate, da interface de administração ou da API).
- **Não disponível:** detecção de movimento e de pessoas; zoom digital; logotipo na imagem;
  gravação na câmera (desativada no perfil qualificado: grave com o NVR) e gerenciamento do
  cartão SD; qualquer acesso pela nuvem ou por aplicativo de celular.

[Comparação com o OEM](../../../docs/OEM-COMPARISON.md) compara este firmware com o
firmware do fabricante, item por item.

<p align="center">
  <img src="../../../docs/images/webui-system.png" width="480"
       alt="A página Sistema do console web, mostrando a Plataforma 0.2.39 e a Mídia 0.1.0 instaladas, além dos controles de atualização, hora, senha, chave SSH e reinicialização">
  <br>
  <sub>A página Sistema: versões da plataforma e da mídia, atualizações, hora, senhas,
  chaves SSH e restauração dos padrões — tudo na câmera, sem nuvem.</sub>
</p>

## Instalação

Siga o guia de [Instalação](./INSTALL.md). A primeira instalação é feita por Wi-Fi,
pelo próprio atualizador do firmware do fabricante, e as atualizações seguintes, pelo
console web. Não é preciso cartão SD, cabo nem gravador de flash, e a câmera continua
fechada. Um gravador de flash só é necessário para recuperar uma câmera que não
inicializa mais ([Recuperação](./RECOVERY.md)).

Para uma instalação guiada, sem terminal, o [instalador](../../../installer/) reúne toda a
sequência — provisionamento, atualização da plataforma, backup, geração e gravação da imagem —
em uma única janela. Ele começa em modo de simulação (dry run), que planeja tudo sem alterar
nada, mostra um log ao vivo e pede confirmação uma só vez, antes da única etapa irreversível.

<p align="center">
  <img src="../../../docs/images/gui-running.png" width="480"
       alt="A janela do instalador: um formulário para o endereço da câmera, pacotes e chaves, uma caixa de seleção Dry run e um log ao vivo, com código de cores, das cinco etapas de instalação">
  <br>
  <sub>O instalador de janela única (<code>python3 installer/gui.py</code>) conduzindo as cinco etapas.</sub>
</p>

## Segurança em resumo

- A câmera precisa ficar em uma rede que bloqueie o acesso dela à Internet. Por alguns
  segundos, a cada inicialização, o servidor web do fabricante roda antes que este
  firmware assuma o controle. O firmware remove as rotas padrão da câmera, mas não tem
  filtro de pacotes; por isso, o isolamento no roteador é a verdadeira fronteira.
- O console web e a API usam HTTP simples na porta 80. Uma requisição precisa se dirigir
  à câmera por um endereço IP privado ou de loopback, ou pelo próprio nome dela (padrão
  `jooan-w3u`, sozinho ou terminado em `.local`, `.lan`, `.home.arpa` ou
  `.internal`). Qualquer outra coisa recebe `403 host_rejected`.
- O acesso é feito com o usuário `admin` e a senha inicial `admin`. Essa senha continua
  válida até ser alterada na página Sistema, e o SSH usa a mesma senha. O daemon de
  mídia tem credenciais próprias, seu próprio certificado TLS e um limitador de
  tentativas de login malsucedidas.
- O `telnetd` e o servidor web do fabricante são encerrados a cada segundo. Quando o
  programa do fabricante roda (somente nos modos de reserva), um mecanismo de contenção
  bloqueia o tráfego dele para a nuvem e mantém seus sockets de escuta restritos ao loopback.

Detalhes: [Segurança de rede](../../../docs/NETWORK-SECURITY.md) e
[Política de segurança](../../../SECURITY.md), que também explica como relatar uma
vulnerabilidade.

## Estrutura do repositório

| Caminho | Conteúdo | Licença |
| --- | --- | --- |
| `build.sh`, `build-uninstall.sh`, `release.sh` | pontos de entrada para gerar versões ([Compilação](../../../docs/BUILDING.md)) | GPL-2.0-only |
| `packaging/` | manifesto do alvo exato e montagem do pacote IronMan | GPL-2.0-only |
| `runtime/` | hook de inicialização, supervisor, scripts de slot e de manutenção | GPL-2.0-only |
| `src/` | `joan-daemon` (console web e API), o mecanismo de contenção, verificadores de versões, ferramentas de empacotamento | GPL-2.0-only |
| `web/` | o console web | GPL-2.0-only |
| `tools/` | `upload_ota.py` (primeira instalação), `make-github-release.py`, download da toolchain, diagnóstico de Wi-Fi | GPL-2.0-only |
| `tests/`, `ci/` | validação no lado do host | GPL-2.0-only |
| `docs/` | documentação | GPL-2.0-only |
| [`media/`](../../../media/) | daemon de mídia `jooanipc-open`, seus scripts de inicialização, ferramentas de appfs e instalador de pacotes de versão | MIT |

A biblioteca `libimp.so` que o daemon de mídia carrega é compilada a partir do fork do
OpenIMP, branch [`jooan/oem-t23-abi`](https://github.com/ADCDS/openimp/tree/jooan/oem-t23-abi),
que é um repositório separado.

O repositório e suas versões não contêm segredos de fábrica, credenciais de Wi-Fi,
chaves do dispositivo, dumps da flash, firmware OEM nem outros binários do fabricante.
Quando é necessário um componente OEM mantido, ele vem da própria câmera do proprietário.
Um pacote da camada de mídia contém uma imagem appfs reconstruída a partir dessa câmera
e, por isso, permanece privado.

## Documentação

- [Instalação](./INSTALL.md): como instalar uma versão publicada.
- [Compatibilidade](../../../docs/COMPATIBILITY.md): como saber se a sua câmera é a
  que tem suporte.
- [Comparação com o OEM](../../../docs/OEM-COMPARISON.md): o que falta neste firmware
  em comparação com o firmware do fabricante.
- [Recuperação](./RECOVERY.md): desinstalação, reversão de versão e recuperação via SPI.
- [Segurança de rede](../../../docs/NETWORK-SECURITY.md): sockets de escuta, verificações
  do cabeçalho Host, Wi-Fi, isolamento.
- [Arquitetura](../../../docs/ARCHITECTURE.md): modos de inicialização, componentes
  mantidos, cobertura de funcionalidades.
- [API](../../../docs/API.md): a API `/api/v1/` da plataforma.
- [Compilação e publicação](../../../docs/BUILDING.md): toolchain, assinatura, limite de
  tamanho, pacotes de versão, lista de verificação de qualificação.
- [Daemon de mídia](../../../media/README.md) e
  [integração com o hardware](../../../media/docs/hardware.md).
- [Plano de testes](../../../tests/TEST_PLAN.md), [Componentes de terceiros](../../../docs/THIRD_PARTY.md),
  [Política de segurança](../../../SECURITY.md).

## Licença

A plataforma (tudo o que está fora de `media/`) é licenciada sob a
[GPL-2.0-only](../../../LICENSE); `media/` está sob a licença MIT ([media/LICENSE](../../../media/LICENSE)).
Cada arquivo traz seu próprio cabeçalho SPDX e mantém sua própria licença, inclusive
quando um script MIT inclui um script GPL em tempo de execução. A plataforma é vinculada
às bibliotecas Mbed TLS da própria câmera nos termos de uma permissão adicional, cujo
texto exato está em [Componentes de terceiros](../../../docs/THIRD_PARTY.md#mbed-tls-linking-exception).
Nenhuma licença é concedida para artefatos OEM ou de terceiros que não estejam no
repositório.
