<!-- translated from README.md 84c6832c04b6 body 319baebb6d4d -->

<p align="center"><img src="../images/icon.svg" width="144" alt="Mario Kart World: Pit Stop icon"></p>

<h1 align="center">Mario Kart World: Pit Stop</h1>

<p align="center">
  <b>Um port homebrew nativo para PlayStation 5 de Mario Kart World (Nintendo Switch 2), por recompilação estática.</b>
</p>

<p align="center">
  <a href="https://github.com/exploitizm/mkw-pit-stop/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_Transferir-mais_recente_para_PS5-0070d1?style=for-the-badge&labelColor=1b2740" alt="Transferir: mais recente para PS5"></a>
</p>

<p align="center">
  <b><a href="https://github.com/exploitizm/mkw-pit-stop/releases">Versões</a></b> ·
  <a href="../../CHANGELOG.md">Alterações</a> ·
  <a href="../../CONTRIBUTING.md">Contribuir</a> ·
  <a href="../../docs/project/roadmap.md">Roteiro</a> ·
  <a href="../../docs/ps5/ps5.md">Notas da PS5</a> ·
  <a href="https://github.com/exploitizm/mkw-pit-stop/issues">Issues</a>
</p>

<!-- i18n-bar:start -->
<p align="center">
  <a href="../../README.md">English</a> ·
  <a href="README.zh-CN.md">简体中文</a> ·
  <a href="README.es.md">Español</a> ·
  <a href="README.ar.md">العربية</a> ·
  <a href="README.hi.md">हिन्दी</a> ·
  <a href="README.pt-BR.md">Português (Brasil)</a> ·
  <b>Português (Portugal)</b> ·
  <a href="README.de.md">Deutsch</a>
</p>
<!-- i18n-bar:end -->

Um port homebrew nativo para PlayStation 5 (`PPSA99193`) de [**mkw-recomp**](https://github.com/adriadam10/mkw-recomp),
a recompilação estática de *Mario Kart World* (Nintendo Switch 2, versões 1.8.0 e 1.0.0) feita por Adrià. Precisas da
tua própria cópia do jogo, extraída da tua própria consola Switch 2: nada do jogo está neste repositório nem nas suas
versões.

**É uma recompilação estática, não um emulador.** O código AArch64 do jogo é traduzido para C com antecedência e
compilado para a CPU x86-64 Zen 2 da PS5 (layout de endereços do convidado `ps5-high144`); não há interpretador de CPU
nem JIT. A GPU também não é emulada: o NVN2, a API gráfica do Switch 2, é reimplementado no nível da API sobre Vulkan com
o controlador RADV do Mesa para a PS5, e os shaders SM86 do jogo são traduzidos para SPIR-V com antecedência no teu PC. As
funções do SDK da Nintendo (`nn::*`) são substituídas pelo nome por código próprio.

> **Projeto para PC (upstream):** procuras a versão para Linux ou Windows? Vê
> [**adriadam10/mkw-recomp**](https://github.com/adriadam10/mkw-recomp) ([instruções para PC abaixo](#jogar-no-pc-linux-e-windows)).

## Status (PS5)

No início, e a avançar depressa. A 18/11/2026, numa PS5 normal:

- **O arranque, os vídeos, os menus e as corridas funcionam**, com saída Vulkan direta pelo RADV, som estéreo pelo
  `sceAudioOut` e até quatro comandos DualSense com sessão iniciada, com vibração e ícones de botões da PlayStation.
- **As corridas correm a 60 fps por predefinição, sem geração de imagens**: cerca de 57-59 fps em média num benchmark de Grand
  Prix, e uma ou outra imagem mais lenta deixa a corrida a 0,95-0,98x do tempo real (veja [Desempenho](#desempenho)).
  O modo de 30 fps do jogo (velocidade total) e a geração de imagens são opções em **Pit Stop > Display**.
- **Multijogador:** o ecrã dividido do jogo (um DualSense por jogador com sessão iniciada) e seu modo sem fio local pela LAN,
  entre PS5 ou com a versão de PC da mesma versão ([docs/ps5/multiplayer.md](../../docs/ps5/multiplayer.md)). Não há jogo online
  da Nintendo.
- **Sobreposição Pit Stop no jogo.** Prime **Options + clique no touchpad** a qualquer momento para abrir o
  **Pit Stop**, um menu de definições pensado para o comando, com linhas rápidas (**Master volume**,
  **Vibration strength**, **Performance HUD**), todas as categorias de definições e remapeamento dos botões do
  DualSense em tempo real, guardado em `/app0/mkw.ini`.
- **Problemas conhecidos ([docs/project/roadmap.md](../../docs/project/roadmap.md)):**
  - *Sessões longas:* a memória livre cai corrida após corrida porque o jogo guarda dados de pistas anteriores; quando
    fica baixa, as texturas podem sair corrompidas ou faltar. Reinicia o título depois de algumas corridas se isso
    acontecer.
  - *Blocos esparsos:* desde o arranque alguns blocos de texturas esparsas não carregam: pequenas falhas de textura
    perto da linha de partida e uma introdução de corrida em preto e branco.
  - *Multijogador:* a primeira corrida numa pista pode expulsar um jogador cujos shaders ainda estão compilando; as
    seguintes funcionam. O ecrã dividido fica abaixo dos 30 fps em cenas cheias.
  - *Menus:* os ecrãs de seleção de modo, personagem e veículo ainda diferem um pouco da consola.

## O que precisas

1. **O teu próprio dump de *Mario Kart World*** da tua própria Nintendo Switch 2: versão **1.8.0** (`exefs/main`, ID
   de build `d00e4f399e411d06`) ou 1.0.0 (`9e244438c3d8ef1c`), como pastas `exefs/` e `romfs/` descriptografadas ou um
   ficheiro `.nup`. Um dump da consola com a atualização 1.8.0 em `updates/` é usado tal como está, e quando há as duas
   é compilada a 1.8.0.
2. **Uma PS5 desbloqueada** com um carregador de payloads homebrew (`etaHEN` ou `elfldr`), `kstuff` e
   **ShadowMountPlus 1.7 Beta 5** ou mais recente (monta e regista `/data/homebrew/PPSA99193` no ecrã inicial). O
   firmware suportado depende do teu desbloqueio, do `kstuff` e do ambiente homebrew do RADV (vê
   [docs/ps5/ps5.md](../../docs/ps5/ps5.md)).
3. **Um PC com cerca de 30 GB livres** na mesma rede da PS5, para compilar o título a partir do teu dump:
   - **Windows 10 ou 11 (64 bits):** nada mais a instalar, sem WSL. O compilador para Windows traz o seu próprio Python,
     zig e glslc ([passos para Windows](#instalação-no-windows-sem-wsl)).
   - **Linux x86-64 ou WSL2:** `clang`, `lld` e Python 3 ([passos para Linux](#instalação-e-atualização-na-ps5-linux-ou-wsl2)).

## Instalação no Windows (sem WSL)

1. **Coloca o título provisório na PS5.** Descompacta `mkw-pit-stop-<version>-ps5-placeholder-PPSA99193.zip` das
   [**versões**](https://github.com/exploitizm/mkw-pit-stop/releases) e envia a pasta `PPSA99193` para `/data/homebrew/`
   com um cliente FTP (FileZilla, WinSCP), de forma a que `/data/homebrew/PPSA99193/eboot.bin` exista. Quando o
   ShadowMountPlus o registar, abre **Mario Kart World: Pit Stop**: ele mostra **Game files missing** e
   `Console IP: <endereço> (port 19913)`. Deixa esse ecrã aberto; a PS5 só recebe ficheiros enquanto ele aparece.
   <p align="center"><img src="../../docs/images/placeholder-ps5.png" width="560" alt="O título provisório na PS5: ficheiros do jogo em falta, o endereço da consola e a etapa e o progresso da compilação no PC"></p>
2. **Descompacta o compilador no PC.** Transfere `mkw-pit-stop-<version>-ps5-builder-windows-x86_64.zip`, clica com o
   botão direito, **Propriedades**, marca **Desbloquear** e extrai numa pasta curta de uma unidade com 30 GB livres,
   como `C:\PitStop` (não `Program Files` nem OneDrive).
3. **Compila e envia.** Executa `mkw.exe` nessa pasta. Em **Game files**, escolhe o teu dump com **Choose folder** (ou
   **Choose .nup or .nx file**); em **Build for PS5**, escreve o endereço do ecrã da PS5 em **PS5 IP** e prime
   **Build for PS5**. O botão de desktop **Set up** não é necessário para a PS5. Ou, numa Linha de Comandos nessa pasta:
   ```bat
   prepare_ps5.bat --source "D:\Dumps\MKW" --send 192.168.1.50
   ```
   <p align="center"><img src="../../docs/images/launcher-ps5.png" width="560" alt="O ecrã inicial do compilador para PS5: a tua cópia escolhida, o endereço da PS5 escrito e o botão Build for PS5"></p>
4. **Inicia o jogo.** Enquanto o PC compila, o ecrã da PS5 mostra a etapa e o progresso; mostra
   **Transfer complete!** quando todos os ficheiros chegaram e foram verificados: prime **X** e abre
   **Mario Kart World: Pit Stop** de novo.

**A tua cópia já está na PS5?** Marca **My game files are already on the PS5** e escreve ali a pasta dela (por exemplo
`/data/mkw`, com `exefs` e `romfs` dentro ou no seu `updates\<version>`): só o código do jogo e os shaders são enviados,
e o título encontra o jogo nessa pasta. O PC continua a precisar do teu dump para compilar.

**Sem rede? Usa uma pen USB.** No passo 3, escolhe **To a USB drive** e **Choose drive** em vez de escrever o endereço
da PS5: o título é gravado em `homebrew\PPSA99193` na unidade (**exFAT**, cerca de 25 GB livres), e o ShadowMountPlus o
encontra ali quando a unidade é ligada à PS5 (ele procura em `/mnt/usb0`-`usb7/homebrew/`). O título provisório do
passo 1 não é necessário para isso; se estiver instalado, apaga `/data/homebrew/PPSA99193` antes, para que só uma cópia
do título fique registada. Compilar de novo atualiza só o que mudou na unidade.

Se a PS5 ficar em **Waiting for connection**, verifica o endereço e se os dois estão na mesma rede; uma compilação que
falhou é explicada em `logs/setup.log`, ao lado de `mkw.exe`. Para atualizar, descompacta o novo compilador e compila de
novo.

## Instalação e atualização na PS5 (Linux ou WSL2)

Como o `eboot.bin` liga o código recompilado do jogo e `spirv/` guarda os shaders traduzidos dele, **nenhum `eboot.bin`
ou `spirv/` pronto é distribuído**. Compilas a pasta do título `PPSA99193` no teu PC a partir do compilador da
versão, que não contém nada do jogo, e do teu próprio dump:

1. Transfere `mkw-pit-stop-<version>-ps5-builder-linux-x86_64.tar.zst` das
   [**versões**](https://github.com/exploitizm/mkw-pit-stop/releases), verifica o hash com `SHA256SUMS` e descompacta no
   Linux ou WSL2:
   ```sh
   tar --zstd -xf mkw-pit-stop-*-ps5-builder-linux-x86_64.tar.zst
   cd mkw-pit-stop-*
   ```
2. Executa `prepare_ps5.py` a apontar para o teu dump de *Mario Kart World* (uma pasta ou um ficheiro `.nup`):
   ```sh
   python3 prepare_ps5.py --source /path/to/your/mkw-dump --out ./PPSA99193
   ```
   O script descobre a versão do teu dump (1.8.0 ou 1.0.0), traduz o ExeFS AArch64 para C (`ps5-high144`), compila e
   liga o `eboot.bin` com o runtime pré-compilado e o pacote RADV da PS5, transfere o `nvdisasm` e o `ptxas` da NVIDIA
   (fixados por SHA-256) para gerar localmente a especificação de instruções SM86, traduz todos os 11.561 shaders para
   `PPSA99193/spirv/` e coloca `exefs/` e `romfs/` em `PPSA99193/`.
3. Transfere `PPSA99193` para `/data/homebrew/PPSA99193` na tua PS5 por FTP ou USB — ou passa `--deploy <PS5_IP>` para
   enviar, verificar cada ficheiro por SHA-256 e renovar o `param.json` e o `icon0.png` registados pelo ShadowMountPlus
   em `/user/app/PPSA99193/sce_sys`.
4. Abre **Mario Kart World: Pit Stop** a partir do ecrã inicial da PS5.

**Atualizar:** descompacta o compilador mais recente e executa `prepare_ps5.py` com as mesmas pastas `--work` e `--out`. As
etapas cujas entradas não mudaram (como `romfs/` e shaders inalterados) são saltadas, e os teus jogos guardados (`profile/`) e
definições (`mkw.ini`) na consola são mantidos.

## Sobreposição de definições Pit Stop

Prime **Options + clique no touchpad** no DualSense (ou **F1** no computador) em qualquer ecrã para abrir o
**Pit Stop** ([docs/ps5/pit-stop.md](../../docs/ps5/pit-stop.md)):

| Comando | Ação |
|---|---|
| **Options + clique no touchpad** | Abrir ou fechar o Pit Stop em qualquer ecrã |
| **Botões direcionais / joystick esquerdo para cima e para baixo** | Mover uma linha |
| **Botões direcionais / joystick esquerdo para a esquerda e direita** | Ajustar a definição selecionada (Desligado/Ligado ou um valor em passos) |
| **Círculo / X** | Confirmar / Voltar (segue a tua disposição de botões Nintendo ou Xbox) |
| **L1 / R1** | Categoria anterior / seguinte em **All settings** |

- **As linhas rápidas** do primeiro ecrã ajustam em tempo real **Master volume** (0–100%), **Vibration strength**
  (0–300%) e **Performance HUD** (Off, FPS, Full).
- **All settings** abre **Display**, **Graphics**, **Controls** (com captura de botões por ação em tempo real),
  **Audio**, **General**, **Advanced**, **Hacks** (desbloquear todos os pilotos, veículos, adesivos e a Copa Especial) e
  **Multiplayer**. As alterações são guardadas automaticamente em `/app0/mkw.ini`.

## Jogar no PC (Linux e Windows)

Do projeto upstream [mkw-recomp](https://github.com/adriadam10/mkw-recomp):

<p align="center">
  <img src="../../docs/images/launcher-files.png" width="400" alt="A página Game files do ecrã inicial antes da preparação">
  <img src="../../docs/images/launcher-main.png" width="400" alt="O ecrã inicial com o jogo já preparado">
</p>

1. Extrai o Mario Kart World 1.0.0 ou 1.8.0 da tua Switch 2 (`exefs/` e `romfs/`, descriptografados).
2. Transfere o launcher de PC das [versões do mkw-recomp](https://github.com/adriadam10/mkw-recomp/releases): no Linux o
   `.AppImage` (guarda o jogo que compila em `~/.local/share/mkw-recomp`; glibc 2.31 ou mais recente), no Windows o
   `.zip`. Os dois precisam de cerca de 30 GB livres.
3. Executa o AppImage ou o `mkw.exe`, abre **Game files**, aponta para o teu dump e prime **Prepare**, depois
   **Play**.

## Compilar a partir do código-fonte

### Alvo nativo da PS5 (`PPSA99193`)

Vê [docs/ps5/ps5.md](../../docs/ps5/ps5.md) e [tools/ps5/README.md](../../tools/ps5/README.md) para as cópias fixadas de
`PS5_VulkanTemplate`, `PS5_Vulkan`, `PS5_Mesa` e `PS5_PayloadSDK`:

```sh
python3 recomp/gen.py --exefs "$MKW_GAME/exefs" --guest-layout ps5-high144 \
    --out build/ps5-game/generated --patches recomp/patches-mkw.txt
python3 tools/shaders/extract.py && python3 tools/shaders/translate.py
python3 tools/ps5/build_game.py --stack "$stack" --include "$headers" \
    --generated build/ps5-game/generated --exefs "$MKW_GAME/exefs"
python3 tools/ps5/deploy.py <PS5_IP> --game
```

Para empacotar uma versão do compilador para PS5 sem nada do jogo a partir do inventário do runtime (`--inventory`):

```sh
python3 tools/ps5/build_game.py --stack "$stack" --include "$headers" --inventory
python3 tools/ps5/release_ps5.py --stack "$stack" --version 0.1.0
```

### Compilações para Linux e Windows

Detalhes em [tools/build/README.md](../../tools/build/README.md):

```sh
python3 recomp/gen.py --exefs game/exefs --out build/generated --jobs 8 --patches recomp/patches-mkw.txt
python3 tools/shaders/extract.py && python3 tools/shaders/translate.py
python3 tools/build/build.py              # build/mkw; --target windows builds mkw.exe with llvm-mingw
build/mkw
```

| Pasta | Conteúdo |
|---|---|
| [recomp/](../../recomp/README.md) | O gerador: AArch64 → C |
| [runtime/](../../runtime/README.md) | O que faz o papel da consola: kernel, memória, substitutos do SDK, NVN2 sobre Vulkan, áudio, entrada, Pit Stop e launcher |
| [sm86/](../../sm86/README.md) | O tradutor de shaders: SASS SM86 → GLSL → SPIR-V |
| [tools/](../../tools/README.md) | Análise, preparação, compilação, empacotamento para PS5, execuções do jogo e diagnóstico |
| [tests/](../../tests/README.md) | Testes sem o jogo nem GPU, na CI em Linux e Windows |
| [docs/](../../docs/README.md) | Arquitetura da PS5, Pit Stop, pesquisa, fontes, roteiro e desempenho |
| [i18n/](../../i18n/README.md) | Os textos do menu como ficheiros gettext (`mkw.pot`, um `.po` por idioma) |
| [server/](../../server/README.md) | O servidor de salas para o jogo sem fio local pela internet (`server/lobby`) |
| `.github/` | CI, modelos de issues e pull requests |

Na raiz: `pyproject.toml` e `uv.lock` (o ambiente Python das ferramentas), `Makefile` (atalhos para as versões e a CI),
`.gitignore`, `LICENSE`, `CONTRIBUTING.md`, `CHANGELOG.md` (as mudanças de cada versão, suas notas), `AGENTS.md` (um
guia para agentes de programação).

## Desempenho

Numa PS5 normal, Mario Kart World 1.8.0, um benchmark de corrida de Grand Prix (`tools/ps5/bench.py`, 210 s de corrida,
1080p, geração de imagens desligada):

| Data | Taxa de imagens | Tempo médio por imagem | Imagem típica | 1% mais baixo | Velocidade da corrida |
|---|---|---|---|---|---|
| 20/11/2026 | 60 fps (a nova predefinição) | 17,0–17,4 ms (57–59 fps) | 16,7 ms (no limite) | 35–41 fps | 0,95–0,98x |
| 16/11/2026 | 30 fps (a predefinição anterior) | 33,3 ms | 33,3 ms | | 1,00x |

A nova predefinição não foi testada na consola tal como é distribuída: estes números vêm de corridas de benchmark com o
registo de desempenho do runtime ligado (`MKW_TRACE=perf`, que custa um pouco), postas a 60 fps à mão. Menus,
contrarrelógio, ecrã dividido, sessões longas e outras pistas ainda não foram jogados com a nova predefinição.

Cerca de 2% das imagens de corrida levam mais de 20 ms: é onde uma pista carrega texturas novas, e são elas que definem o 1% mais
baixo e a ligeira câmara lenta. O que foi medido e o que falta está em [docs/project/roadmap.md](../../docs/project/roadmap.md) e
[docs/ps5/ps5.md](../../docs/ps5/ps5.md#frame-rate); os números de PC, em [docs/project/performance.md](../../docs/project/performance.md).

## Comunicar erros

Vê [CONTRIBUTING.md](../../CONTRIBUTING.md). Comunica problemas da PS5 nas
[issues deste fork](https://github.com/exploitizm/mkw-pit-stop/issues) e anexa o registo do kernel da consola (`klog`, com o
prefixo `[Mario Kart World: Pit Stop]`) e o `logs/setup.log` do compilador de PC. Para problemas no computador, anexa
`logs/mkw.log` e `logs/setup.log`. Nunca anexes ficheiros do jogo, dumps ou binários `eboot.bin` compilados a partir
do teu dump.

## Licenças, créditos e aviso legal

O código-fonte deste repositório está sob a [licença MIT](../../LICENSE) (`Copyright (c) 2026 Adrià`).

**Os ficheiros das versões para PS5 são GPL-3.0-or-later.** Cada título de PS5 liga a camada de plataforma do fork do SDK
de payloads e o código de inicialização do `PS5_Vulkan` (GPL-3.0-or-later), então o título provisório, o `ps5-bundle/`
do compilador e cada título que compilares com eles ficam sob GPL-3.0-or-later como um todo; o código próprio do
mkw-recomp continua MIT. Cada versão traz `LEGAL.txt` e `licenses/` (cada parte com sua licença, copyright e revisão de
origem) e anexa o código-fonte completo de cada parte como `source/*.tar.gz` (`source/SOURCES.txt`), gerados por
[tools/ps5/release_notices.py](../../tools/ps5/release_notices.py).

- **Projeto upstream:** [**mkw-recomp**](https://github.com/adriadam10/mkw-recomp) de Adrià — o tradutor AArch64→C, o
  kernel do Switch 2 e os substitutos do SDK `nn::*`, o renderizador NVN2 sobre Vulkan e o tradutor de shaders
  SM86→SPIR-V offline.
- **Projetos MIT e permissivos** (ficheiros `LICENSE.*` em [recomp/](../../recomp/), [runtime/](../../runtime/),
  [sm86/decode/](../../sm86/decode/) e [runtime/third_party/](../../runtime/third_party/README.md)):
  [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) e seu port para Linux,
  [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp),
  [DocumentSASS](https://github.com/0xD0GF00D/DocumentSASS) e
  [nvsass-disassembler](https://github.com/reoLantern/nvsass-disassembler),
  [Ryujinx](https://git.ryujinx.app/ryubing/ryujinx), [Mesa NAK](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/nouveau/compiler),
  [mario-kart-world-save-editor](https://github.com/Ca1nM/mario-kart-world-save-editor), Dear ImGui,
  [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), AMD FidelityFX FSR 1, Vulkan Memory Allocator, Roboto e
  Lato.
- **Ferramentas e runtime da PS5:** [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate) (MIT, Sascha
  Willems e Mihawk), [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) (MIT, RADV para PS5),
  [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) e [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK)
  (GPL-3.0-or-later, Mihawk e [John Törnblom / ps5-payload-dev](https://github.com/ps5-payload-dev/sdk)), e
  [ShadowMountPlus](https://github.com/drakmor/shadowMountPlus). Quando ligado a um título de PS5 com a camada de
  plataforma do SDK de payloads e a CRT e os stubs do `PS5_Vulkan`, o binário combinado do título de PS5 fica sob
  GPL-3.0-or-later (`LEGAL.txt` e `licenses/` dentro do título compilado).
- **Só pesquisa e ideias de design** (nenhum código copiado de projetos GPL ou sem licença): eden-nx2, Hookshot,
  PS5_Hookshot, EglantinePS5, AnyPS5, prosper e shadPS4. Cada fonte está documentada em
  [docs/project/research.md](../../docs/project/research.md#sources).

**Aviso legal:** *Mario Kart World* e *Mario* são marcas registadas da Nintendo. «PlayStation» e «PS5» são marcas
registadas da Sony Interactive Entertainment Inc. Este projeto não está afiliado nem é apoiado pela Nintendo, pela Sony
Interactive Entertainment nem por nenhuma editora de jogos, não contém código, shaders ou recursos do jogo e não ajuda a
obtê-los.
