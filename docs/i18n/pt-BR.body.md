Um port homebrew nativo para PlayStation 5 (`PPSA99193`) de [**mkw-recomp**](https://github.com/adriadam10/mkw-recomp),
a recompilação estática de *Mario Kart World* (Nintendo Switch 2, versões 1.8.0 e 1.0.0) feita por Adrià. Você precisa
da sua própria cópia do jogo, extraída do seu próprio console Switch 2: nada do jogo está neste repositório nem nas suas
versões.

**É uma recompilação estática, não um emulador.** O código AArch64 do jogo é traduzido para C com antecedência e
compilado para a CPU x86-64 Zen 2 do PS5 (layout de endereços do convidado `ps5-high144`); não há interpretador de CPU
nem JIT. A GPU também não é emulada: o NVN2, a API gráfica do Switch 2, é reimplementado no nível da API sobre Vulkan com
o driver RADV do Mesa para PS5, e os shaders SM86 do jogo são traduzidos para SPIR-V com antecedência no seu PC. As
funções do SDK da Nintendo (`nn::*`) são substituídas pelo nome por código próprio.

> **Projeto para PC (upstream):** procurando a versão para Linux ou Windows? Veja
> [**adriadam10/mkw-recomp**](https://github.com/adriadam10/mkw-recomp) ([instruções para PC abaixo](#@pc)).

## Status (PS5)

No começo, e avançando rápido. Em 18/11/2026, num PS5 padrão:

- **Inicialização, vídeos, menus e corridas funcionam**, com saída Vulkan direta pelo RADV, som estéreo pelo
  `sceAudioOut` e até quatro controles DualSense conectados a contas, com vibração e ícones de botões do PlayStation.
- **As corridas rodam a 60 fps por padrão, sem geração de quadros**: cerca de 57-59 fps em média num benchmark de Grand
  Prix, e um ou outro quadro mais lento deixa a corrida a 0,95-0,98x do tempo real (veja [Desempenho](#@performance)).
  O modo de 30 fps do jogo (velocidade total) e a geração de quadros são opções em **Pit Stop > Display**.
- **Multijogador:** a tela dividida do jogo (um DualSense por jogador conectado) e seu modo sem fio local pela LAN,
  entre PS5 ou com a versão de PC da mesma versão ([docs/ps5/multiplayer.md](docs/ps5/multiplayer.md)). Não há jogo online
  da Nintendo.
- **Sobreposição Pit Stop no jogo.** Pressione **Options + clique no touchpad** a qualquer momento para abrir o
  **Pit Stop**, um menu de configurações feito para o controle, com linhas rápidas (**Master volume**,
  **Vibration strength**, **Performance HUD**), todas as categorias de configurações e remapeamento dos botões do
  DualSense em tempo real, salvo em `/app0/mkw.ini`.
- **Problemas conhecidos ([docs/ps5/roadmap.md](docs/ps5/roadmap.md)):**
  - *Sessões longas:* a memória livre cai corrida após corrida porque o jogo guarda dados de pistas anteriores; quando
    fica baixa, as texturas podem sair corrompidas ou faltar. Reinicie o título depois de algumas corridas se isso
    acontecer.
  - *Blocos esparsos:* desde a inicialização alguns blocos de texturas esparsas não carregam: pequenas falhas de textura
    perto da largada e uma introdução de corrida em preto e branco.
  - *Multijogador:* a primeira corrida numa pista pode derrubar um jogador cujos shaders ainda estão compilando; as
    seguintes funcionam. A tela dividida fica abaixo de 30 fps em cenas cheias.
  - *Menus:* as telas de seleção de modo, personagem e veículo ainda diferem um pouco do console.

## O que você precisa

1. **Sua própria extração de *Mario Kart World*** do seu próprio Nintendo Switch 2: versão **1.8.0** (`exefs/main`, ID
   de build `d00e4f399e411d06`) ou 1.0.0 (`9e244438c3d8ef1c`), como pastas `exefs/` e `romfs/` descriptografadas ou um
   arquivo `.nup`. Uma extração do console com a atualização 1.8.0 em `updates/` é usada como está, e quando há as duas
   a 1.8.0 é compilada.
2. **Um PS5 desbloqueado** com um carregador de payloads homebrew (`etaHEN` ou `elfldr`), `kstuff` e
   **ShadowMountPlus 1.7 Beta 5** ou mais recente (monta e registra `/data/homebrew/PPSA99193` na tela inicial). O
   firmware suportado depende do seu desbloqueio, do `kstuff` e do ambiente homebrew do RADV (veja
   [docs/ps5/ps5.md](docs/ps5/ps5.md)).
3. **Um PC com cerca de 30 GB livres** na mesma rede do PS5, para compilar o título a partir da sua extração:
   - **Windows 10 ou 11 (64 bits):** nada mais para instalar, sem WSL. O compilador para Windows traz seu próprio Python,
     zig e glslc ([passos para Windows](#@windows)).
   - **Linux x86-64 ou WSL2:** `clang`, `lld` e Python 3 ([passos para Linux](#@linux)).

## Instalação no Windows (sem WSL)

1. **Coloque o título provisório no PS5.** Descompacte `mkw-pit-stop-<version>-ps5-placeholder-PPSA99193.zip` das
   [**versões**](https://github.com/exploitizm/mkw-pit-stop/releases) e envie a pasta `PPSA99193` para `/data/homebrew/`
   com um cliente FTP (FileZilla, WinSCP), de forma que `/data/homebrew/PPSA99193/eboot.bin` exista. Quando o
   ShadowMountPlus o registrar, abra **Mario Kart World: Pit Stop**: ele mostra **Game files missing** e
   `Console IP: <endereço> (port 19913)`. Deixe essa tela aberta; o PS5 só recebe arquivos enquanto ela aparece.
   <p align="center"><img src="docs/images/placeholder-ps5.png" width="560" alt="O título provisório no PS5: arquivos do jogo ausentes, o endereço do console e a etapa e o progresso da compilação no PC"></p>
2. **Descompacte o compilador no PC.** Baixe `mkw-pit-stop-<version>-ps5-builder-windows-x86_64.zip`, clique com o
   botão direito, **Propriedades**, marque **Desbloquear** e extraia numa pasta curta de uma unidade com 30 GB livres,
   como `C:\PitStop` (não `Program Files` nem OneDrive).
3. **Compile e envie.** Rode `mkw.exe` nessa pasta. Em **Game files**, escolha sua extração com **Choose folder** (ou
   **Choose .nup or .nx file**); em **Build for PS5**, digite o endereço da tela do PS5 em **PS5 IP** e pressione
   **Build for PS5**. O botão de desktop **Set up** não é necessário para o PS5. Ou, num Prompt de Comando nessa pasta:
   ```bat
   prepare_ps5.bat --source "D:\Dumps\MKW" --send 192.168.1.50
   ```
   <p align="center"><img src="docs/images/launcher-ps5.png" width="560" alt="A tela inicial do compilador para PS5: sua cópia escolhida, o endereço do PS5 digitado e o botão Build for PS5"></p>
4. **Inicie o jogo.** Enquanto o PC compila, a tela do PS5 mostra a etapa e o progresso; ela mostra
   **Transfer complete!** quando todos os arquivos chegaram e foram conferidos: pressione **X** e abra
   **Mario Kart World: Pit Stop** de novo.

**Sua cópia já está no PS5?** Marque **My game files are already on the PS5** e digite a pasta dela ali (por exemplo
`/data/mkw`, com `exefs` e `romfs` dentro ou no seu `updates\<version>`): só o código do jogo e os shaders são enviados,
e o título encontra o jogo nessa pasta. O PC ainda precisa da sua extração para compilar.

**Sem rede? Use um pendrive.** No passo 3, escolha **To a USB drive** e **Choose drive** em vez de digitar o endereço
do PS5: o título é gravado em `homebrew\PPSA99193` na unidade (**exFAT**, cerca de 25 GB livres), e o ShadowMountPlus o
encontra ali quando a unidade é ligada ao PS5 (ele procura em `/mnt/usb0`-`usb7/homebrew/`). O título provisório do
passo 1 não é necessário para isso; se estiver instalado, apague `/data/homebrew/PPSA99193` antes, para que só uma cópia
do título fique registrada. Compilar de novo atualiza só o que mudou na unidade.

Se o PS5 ficar em **Waiting for connection**, confira o endereço e se os dois estão na mesma rede; uma compilação que
falhou é explicada em `logs/setup.log`, ao lado de `mkw.exe`. Para atualizar, descompacte o novo compilador e compile de
novo.

## Instalação e atualização no PS5 (Linux ou WSL2)

Como o `eboot.bin` liga o código recompilado do jogo e `spirv/` guarda os shaders traduzidos dele, **nenhum `eboot.bin`
ou `spirv/` pronto é distribuído**. Você compila a pasta do título `PPSA99193` no seu PC a partir do compilador da
versão, que não contém nada do jogo, e da sua própria extração:

1. Baixe `mkw-pit-stop-<version>-ps5-builder-linux-x86_64.tar.zst` das
   [**versões**](https://github.com/exploitizm/mkw-pit-stop/releases), confira o hash com `SHA256SUMS` e descompacte no
   Linux ou WSL2:
   ```sh
   tar --zstd -xf mkw-pit-stop-*-ps5-builder-linux-x86_64.tar.zst
   cd mkw-pit-stop-*
   ```
2. Rode `prepare_ps5.py` apontando para sua extração de *Mario Kart World* (uma pasta ou um arquivo `.nup`):
   ```sh
   python3 prepare_ps5.py --source /path/to/your/mkw-dump --out ./PPSA99193
   ```
   O script descobre a versão da sua extração (1.8.0 ou 1.0.0), traduz o ExeFS AArch64 para C (`ps5-high144`), compila e
   liga o `eboot.bin` com o runtime pré-compilado e o arquivo RADV do PS5, baixa o `nvdisasm` e o `ptxas` da NVIDIA
   (fixados por SHA-256) para gerar localmente a especificação de instruções SM86, traduz todos os 11.561 shaders para
   `PPSA99193/spirv/` e coloca `exefs/` e `romfs/` em `PPSA99193/`.
3. Transfira `PPSA99193` para `/data/homebrew/PPSA99193` no seu PS5 por FTP ou USB — ou passe `--deploy <PS5_IP>` para
   enviar, conferir cada arquivo por SHA-256 e renovar o `param.json` e o `icon0.png` registrados pelo ShadowMountPlus
   em `/user/app/PPSA99193/sce_sys`.
4. Abra **Mario Kart World: Pit Stop** pela tela inicial do PS5.

**Atualizar:** descompacte o compilador mais novo e rode `prepare_ps5.py` com as mesmas pastas `--work` e `--out`. As
etapas cujas entradas não mudaram (como `romfs/` e shaders inalterados) são puladas, e seus jogos salvos (`profile/`) e
configurações (`mkw.ini`) no console são mantidos.

## Sobreposição de configurações Pit Stop

Pressione **Options + clique no touchpad** no DualSense (ou **F1** no desktop) em qualquer tela para abrir o
**Pit Stop** ([docs/ps5/pit-stop.md](docs/ps5/pit-stop.md)):

| Controle | Ação |
|---|---|
| **Options + clique no touchpad** | Abrir ou fechar o Pit Stop em qualquer tela |
| **Direcional / analógico esquerdo para cima e para baixo** | Mover uma linha |
| **Direcional / analógico esquerdo para a esquerda e direita** | Ajustar a configuração selecionada (Desligado/Ligado ou um valor em passos) |
| **Círculo / X** | Confirmar / Voltar (segue seu layout de botões Nintendo ou Xbox) |
| **L1 / R1** | Categoria anterior / seguinte em **All settings** |

- **As linhas rápidas** da primeira tela ajustam em tempo real **Master volume** (0–100%), **Vibration strength**
  (0–300%) e **Performance HUD** (Off, FPS, Full).
- **All settings** abre **Display**, **Graphics**, **Controls** (com captura de botões por ação em tempo real),
  **Audio**, **General**, **Advanced**, **Hacks** (desbloquear todos os pilotos, veículos, adesivos e a Copa Especial) e
  **Multiplayer**. As mudanças são salvas automaticamente em `/app0/mkw.ini`.

## Jogar no PC (Linux e Windows)

Do projeto upstream [mkw-recomp](https://github.com/adriadam10/mkw-recomp):

<p align="center">
  <img src="docs/images/launcher-files.png" width="400" alt="A página Game files da tela inicial antes da preparação">
  <img src="docs/images/launcher-main.png" width="400" alt="A tela inicial com o jogo já preparado">
</p>

1. Extraia o Mario Kart World 1.0.0 ou 1.8.0 do seu Switch 2 (`exefs/` e `romfs/`, descriptografados).
2. Baixe o launcher de PC nas [versões do mkw-recomp](https://github.com/adriadam10/mkw-recomp/releases): no Linux o
   `.AppImage` (guarda o jogo que compila em `~/.local/share/mkw-recomp`; glibc 2.31 ou mais recente), no Windows o
   `.zip`. Os dois precisam de cerca de 30 GB livres.
3. Rode o AppImage ou o `mkw.exe`, abra **Game files**, aponte para sua extração e pressione **Prepare**, depois
   **Play**.

## Compilar a partir do código-fonte

### Alvo nativo do PS5 (`PPSA99193`)

Veja [docs/ps5/ps5.md](docs/ps5/ps5.md) e [tools/ps5/README.md](tools/ps5/README.md) para as cópias fixadas de
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

Detalhes em [tools/build/README.md](tools/build/README.md):

```sh
python3 recomp/gen.py --exefs game/exefs --out build/generated --jobs 8 --patches recomp/patches-mkw.txt
python3 tools/shaders/extract.py && python3 tools/shaders/translate.py
python3 tools/build/build.py              # build/mkw; --target windows builds mkw.exe with llvm-mingw
build/mkw
```

| Pasta | Conteúdo |
|---|---|
| [recomp/](recomp/README.md) | O gerador: AArch64 → C |
| [runtime/](runtime/README.md) | O que faz o papel do console: kernel, memória, substitutos do SDK, NVN2 sobre Vulkan, áudio, entrada, Pit Stop e launcher |
| [sm86/](sm86/README.md) | O tradutor de shaders: SASS SM86 → GLSL → SPIR-V |
| [tools/](tools/README.md) | Análise, preparação, compilação, empacotamento para PS5, execuções do jogo e diagnóstico |
| [tests/](tests/README.md) | Testes sem o jogo nem GPU, na CI no Linux e no Windows |
| [docs/](docs/README.md) | Arquitetura do PS5, Pit Stop, pesquisa, fontes, roteiro e desempenho |
| [i18n/](i18n/README.md) | Os textos do menu como arquivos gettext (`mkw.pot`, um `.po` por idioma) |
| [server/](server/README.md) | O servidor de salas para o jogo sem fio local pela internet (`server/lobby`) |
| `.github/` | CI, modelos de issues e pull requests |

Na raiz: `pyproject.toml` e `uv.lock` (o ambiente Python das ferramentas), `Makefile` (atalhos para as versões e a CI),
`.gitignore`, `LICENSE`, `CONTRIBUTING.md`, `CHANGELOG.md` (as mudanças de cada versão, suas notas), `AGENTS.md` (um
guia para agentes de programação).

## Desempenho

Num PS5 padrão, Mario Kart World 1.8.0, um benchmark de corrida de Grand Prix (`tools/ps5/bench.py`, 210 s de corrida,
1080p, geração de quadros desligada):

| Data | Taxa de quadros | Tempo médio por quadro | Quadro típico | 1% mais baixo | Velocidade da corrida |
|---|---|---|---|---|---|
| 20/11/2026 | 60 fps (o novo padrão) | 17,0–17,4 ms (57–59 fps) | 16,7 ms (no limite) | 35–41 fps | 0,95–0,98x |
| 16/11/2026 | 30 fps (o padrão anterior) | 33,3 ms | 33,3 ms | | 1,00x |

O novo padrão não foi testado no console do jeito que é distribuído: estes números vêm de corridas de benchmark com o
registro de desempenho do runtime ligado (`MKW_TRACE=perf`, que custa um pouco), colocadas em 60 fps à mão. Menus,
contrarrelógio, tela dividida, sessões longas e outras pistas ainda não foram jogados com o novo padrão.

Cerca de 2% dos quadros de corrida levam mais de 20 ms: é onde uma pista carrega texturas novas, e eles definem o 1% mais
baixo e a leve câmera lenta. O que foi medido e o que falta está em [docs/ps5/roadmap.md](docs/ps5/roadmap.md) e
[docs/ps5/ps5.md](docs/ps5/ps5.md#frame-rate); os números de PC, em [docs/project/performance.md](docs/project/performance.md).

## Relatar erros

Veja [CONTRIBUTING.md](CONTRIBUTING.md). Relate problemas do PS5 nas
[issues deste fork](https://github.com/exploitizm/mkw-pit-stop/issues) e anexe o log do kernel do console (`klog`, com o
prefixo `[Mario Kart World: Pit Stop]`) e o `logs/setup.log` do compilador de PC. Para problemas de desktop, anexe
`logs/mkw.log` e `logs/setup.log`. Nunca anexe arquivos do jogo, extrações ou binários `eboot.bin` compilados a partir
da sua extração.

## Licenças, créditos e aviso legal

O código-fonte deste repositório está sob a [licença MIT](LICENSE) (`Copyright (c) 2026 Adrià`).

**Os arquivos das versões para PS5 são GPL-3.0-or-later.** Cada título de PS5 liga a camada de plataforma do fork do SDK
de payloads e o código de inicialização do `PS5_Vulkan` (GPL-3.0-or-later), então o título provisório, o `ps5-bundle/`
do compilador e cada título que você compilar com eles ficam sob GPL-3.0-or-later como um todo; o código próprio do
mkw-recomp continua MIT. Cada versão traz `LEGAL.txt` e `licenses/` (cada parte com sua licença, copyright e revisão de
origem) e anexa o código-fonte completo de cada parte como `source/*.tar.gz` (`source/SOURCES.txt`), gerados por
[tools/ps5/release_notices.py](tools/ps5/release_notices.py).

- **Projeto upstream:** [**mkw-recomp**](https://github.com/adriadam10/mkw-recomp) de Adrià — o tradutor AArch64→C, o
  kernel do Switch 2 e os substitutos do SDK `nn::*`, o renderizador NVN2 sobre Vulkan e o tradutor de shaders
  SM86→SPIR-V offline.
- **Projetos MIT e permissivos** (arquivos `LICENSE.*` em [recomp/](recomp/), [runtime/](runtime/),
  [sm86/decode/](sm86/decode/) e [runtime/third_party/](runtime/third_party/README.md)):
  [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) e seu port para Linux,
  [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp),
  [DocumentSASS](https://github.com/0xD0GF00D/DocumentSASS) e
  [nvsass-disassembler](https://github.com/reoLantern/nvsass-disassembler),
  [Ryujinx](https://git.ryujinx.app/ryubing/ryujinx), [Mesa NAK](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/nouveau/compiler),
  [mario-kart-world-save-editor](https://github.com/Ca1nM/mario-kart-world-save-editor), Dear ImGui,
  [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), AMD FidelityFX FSR 1, Vulkan Memory Allocator, Roboto e
  Lato.
- **Ferramentas e runtime do PS5:** [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate) (MIT, Sascha
  Willems e Mihawk), [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) (MIT, RADV para PS5),
  [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) e [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK)
  (GPL-3.0-or-later, Mihawk e [John Törnblom / ps5-payload-dev](https://github.com/ps5-payload-dev/sdk)), e
  [ShadowMountPlus](https://github.com/drakmor/shadowMountPlus). Quando ligado a um título de PS5 com a camada de
  plataforma do SDK de payloads e a CRT e os stubs do `PS5_Vulkan`, o binário combinado do título de PS5 fica sob
  GPL-3.0-or-later (`LEGAL.txt` e `licenses/` dentro do título compilado).
- **Só pesquisa e ideias de design** (nenhum código copiado de projetos GPL ou sem licença): eden-nx2, Hookshot,
  PS5_Hookshot, EglantinePS5, AnyPS5, prosper e shadPS4. Cada fonte está documentada em
  [docs/project/research.md](docs/project/research.md#sources).

**Aviso legal:** *Mario Kart World* e *Mario* são marcas registradas da Nintendo. "PlayStation" e "PS5" são marcas
registradas da Sony Interactive Entertainment Inc. Este projeto não é afiliado nem endossado pela Nintendo, pela Sony
Interactive Entertainment nem por nenhuma editora de jogos, não contém código, shaders ou recursos do jogo e não ajuda a
obtê-los.
