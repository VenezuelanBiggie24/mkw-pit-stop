<!-- translated from README.md 84c6832c04b6 body b875c8a77363 -->

<p align="center"><img src="../images/icon.svg" width="144" alt="Mario Kart World: Pit Stop icon"></p>

<h1 align="center">Mario Kart World: Pit Stop</h1>

<p align="center">
  <b>Un port homebrew nativo para PlayStation 5 de Mario Kart World (Nintendo Switch 2), por recompilación estática.</b>
</p>

<p align="center">
  <a href="https://github.com/exploitizm/mkw-pit-stop/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_Descargar-%C3%BAltima_para_PS5-0070d1?style=for-the-badge&labelColor=1b2740" alt="Descargar: última para PS5"></a>
</p>

<p align="center">
  <b><a href="https://github.com/exploitizm/mkw-pit-stop/releases">Versiones</a></b> ·
  <a href="../../CHANGELOG.md">Cambios</a> ·
  <a href="../../CONTRIBUTING.md">Contribuir</a> ·
  <a href="../../docs/project/roadmap.md">Hoja de ruta</a> ·
  <a href="../../docs/ps5/ps5.md">Notas de PS5</a> ·
  <a href="https://github.com/exploitizm/mkw-pit-stop/issues">Issues</a>
</p>

<!-- i18n-bar:start -->
<p align="center">
  <a href="../../README.md">English</a> ·
  <a href="README.zh-CN.md">简体中文</a> ·
  <b>Español</b> ·
  <a href="README.ar.md">العربية</a> ·
  <a href="README.hi.md">हिन्दी</a> ·
  <a href="README.pt-BR.md">Português (Brasil)</a> ·
  <a href="README.pt-PT.md">Português (Portugal)</a> ·
  <a href="README.de.md">Deutsch</a>
</p>
<!-- i18n-bar:end -->

Un port homebrew nativo para PlayStation 5 (`PPSA99193`) de [**mkw-recomp**](https://github.com/adriadam10/mkw-recomp),
la recompilación estática de *Mario Kart World* (Nintendo Switch 2, versiones 1.8.0 y 1.0.0) hecha por Adrià. Necesitas
tu propia copia del juego, volcada desde tu propia consola Switch 2: nada del juego está en este repositorio ni en sus
versiones.

**Es una recompilación estática, no un emulador.** El código AArch64 del juego se traduce a C de antemano y se compila
para la CPU x86-64 Zen 2 de la PS5 (disposición de direcciones del invitado `ps5-high144`); no hay intérprete de CPU ni
JIT. Tampoco se emula la GPU: NVN2, la API gráfica de Switch 2, está reimplementada a nivel de API sobre Vulkan con el
controlador RADV de Mesa para PS5, y los shaders SM86 del juego se traducen a SPIR-V de antemano en tu PC. Las funciones
del SDK de Nintendo (`nn::*`) se sustituyen por su nombre con código propio.

> **Proyecto para PC (upstream):** ¿buscas la versión para Linux o Windows? Consulta
> [**adriadam10/mkw-recomp**](https://github.com/adriadam10/mkw-recomp) ([instrucciones para PC más abajo](#jugar-en-pc-linux-y-windows)).

## Estado (PS5)

Temprano, y avanzando rápido. A 18/11/2026 en una PS5 estándar:

- **El arranque, los vídeos, los menús y las carreras funcionan**, con salida Vulkan directa sobre RADV, sonido estéreo
  por `sceAudioOut` y hasta cuatro mandos DualSense con sesión iniciada, con vibración e iconos de botones de PlayStation.
- **Las carreras van a 60 fps por defecto, sin generación de fotogramas**: unos 57-59 fps de media en un benchmark de
  Gran Premio, y algún fotograma más lento deja la carrera a 0,95-0,98x del tiempo real (ver
  [Rendimiento](#rendimiento)). El modo de 30 fps del juego (velocidad completa) y la generación de fotogramas son
  opciones en **Pit Stop > Display**.
- **Multijugador:** la pantalla dividida del juego (un DualSense por jugador con sesión iniciada) y su modo inalámbrico
  local por la LAN, entre PS5 o con la versión de PC de la misma versión ([docs/ps5/multiplayer.md](../../docs/ps5/multiplayer.md)).
  No hay juego en línea de Nintendo.
- **Superposición Pit Stop en el juego.** Pulsa **Options + clic del panel táctil** en cualquier momento para abrir
  **Pit Stop**, un menú de ajustes pensado para el mando con filas rápidas (**Master volume**, **Vibration strength**,
  **Performance HUD**), todas las categorías de ajustes y reasignación de botones del DualSense en vivo, guardado en
  `/app0/mkw.ini`.
- **Problemas conocidos ([docs/project/roadmap.md](../../docs/project/roadmap.md)):**
  - *Sesiones largas:* la memoria libre baja carrera tras carrera porque el juego conserva datos de circuitos
    anteriores; cuando escasea, las texturas pueden salir corruptas o faltar. Reinicia el título tras unas cuantas
    carreras si pasa.
  - *Teselas dispersas:* desde el arranque algunas teselas de texturas dispersas no se cargan: pequeños fallos de textura
    cerca de la línea de salida y una intro de carrera en blanco y negro.
  - *Multijugador:* la primera carrera en un circuito puede expulsar a un jugador cuyos shaders aún se compilan; las
    siguientes funcionan. La pantalla dividida va por debajo de 30 fps en escenas cargadas.
  - *Menús:* las pantallas de selección de modo, personaje y vehículo aún difieren ligeramente de la consola.

## Qué necesitas

1. **Tu propio volcado de *Mario Kart World*** de tu propia Nintendo Switch 2: versión **1.8.0** (`exefs/main`, ID de
   compilación `d00e4f399e411d06`) o 1.0.0 (`9e244438c3d8ef1c`), como carpetas `exefs/` y `romfs/` descifradas o un
   archivo `.nup`. Un volcado de consola con la actualización 1.8.0 en `updates/` se usa tal cual, y si están ambas se
   compila la 1.8.0.
2. **Una PS5 con jailbreak** con un cargador de payloads homebrew (`etaHEN` o `elfldr`), `kstuff` y
   **ShadowMountPlus 1.7 Beta 5** o posterior (monta y registra `/data/homebrew/PPSA99193` en la pantalla de inicio).
   El firmware compatible depende de tu jailbreak, de `kstuff` y del entorno homebrew de RADV (ver
   [docs/ps5/ps5.md](../../docs/ps5/ps5.md)).
3. **Un PC con unos 30 GB libres** en la misma red que la PS5, para compilar el título a partir de tu volcado:
   - **Windows 10 u 11 (64 bits):** no hay que instalar nada más, sin WSL. El compilador para Windows trae su propio
     Python, zig y glslc ([pasos para Windows](#instalación-en-windows-sin-wsl)).
   - **Linux x86-64 o WSL2:** `clang`, `lld` y Python 3 ([pasos para Linux](#instalación-y-actualización-en-ps5-linux-o-wsl2)).

## Instalación en Windows (sin WSL)

1. **Pon el título provisional en la PS5.** Descomprime `mkw-pit-stop-<version>-ps5-placeholder-PPSA99193.zip` de las
   [**versiones**](https://github.com/exploitizm/mkw-pit-stop/releases) y sube su carpeta `PPSA99193` a
   `/data/homebrew/` con un cliente FTP (FileZilla, WinSCP), de modo que exista `/data/homebrew/PPSA99193/eboot.bin`.
   Cuando ShadowMountPlus lo registre, abre **Mario Kart World: Pit Stop**: muestra **Game files missing** y
   `Console IP: <dirección> (port 19913)`. Deja esa pantalla abierta; la PS5 solo recibe archivos mientras se muestra.
   <p align="center"><img src="../../docs/images/placeholder-ps5.png" width="560" alt="El título provisional en la PS5: faltan los archivos del juego, la dirección de la consola y el paso y progreso de la compilación en el PC"></p>
2. **Descomprime el compilador en el PC.** Descarga `mkw-pit-stop-<version>-ps5-builder-windows-x86_64.zip`, haz clic
   derecho, **Propiedades**, marca **Desbloquear** y extráelo en una carpeta corta de una unidad con 30 GB libres, como
   `C:\PitStop` (no `Program Files` ni OneDrive).
3. **Compila y envía.** Ejecuta `mkw.exe` desde esa carpeta. En **Game files**, elige tu volcado con **Choose folder**
   (o **Choose .nup or .nx file**); en **Build for PS5**, escribe la dirección de la pantalla de la PS5 en **PS5 IP** y
   pulsa **Build for PS5**. El botón de escritorio **Set up** no hace falta para la PS5. O, desde un símbolo del sistema
   en esa carpeta:
   ```bat
   prepare_ps5.bat --source "D:\Dumps\MKW" --send 192.168.1.50
   ```
   <p align="center"><img src="../../docs/images/launcher-ps5.png" width="560" alt="La pantalla de inicio del compilador para PS5: tu copia elegida, la dirección de la PS5 escrita y el botón Build for PS5"></p>
4. **Inicia el juego.** Mientras el PC compila, la pantalla de la PS5 muestra el paso y el progreso; muestra
   **Transfer complete!** cuando todos los archivos han llegado y se han comprobado: pulsa **Cruz** y vuelve a abrir
   **Mario Kart World: Pit Stop**.

**¿Tu copia ya está en la PS5?** Marca **My game files are already on the PS5** y escribe allí su carpeta (por ejemplo
`/data/mkw`, con `exefs` y `romfs` dentro o en su `updates\<version>`): solo se envían el código del juego y los
shaders, y el título encuentra el juego en esa carpeta. El PC sigue necesitando tu volcado para compilar.

**¿Sin red? Usa una unidad USB.** En el paso 3, elige **To a USB drive** y **Choose drive** en lugar de escribir la
dirección de la PS5: el título se escribe en `homebrew\PPSA99193` de la unidad (**exFAT**, unos 25 GB libres) y
ShadowMountPlus lo encuentra allí cuando la unidad se conecta a la PS5 (busca en `/mnt/usb0`-`usb7/homebrew/`). El
título provisional del paso 1 no hace falta para esto; si está instalado, borra antes `/data/homebrew/PPSA99193` para
que solo haya una copia registrada. Volver a compilar actualiza solo lo que cambió en la unidad.

Si la PS5 se queda en **Waiting for connection**, comprueba la dirección y que ambos estén en la misma red; una
compilación fallida se explica en `logs/setup.log` junto a `mkw.exe`. Para actualizar, descomprime el nuevo compilador
y vuelve a compilar.

## Instalación y actualización en PS5 (Linux o WSL2)

Como `eboot.bin` enlaza el código recompilado del juego y `spirv/` contiene sus shaders traducidos, **no se distribuye
ningún `eboot.bin` ni `spirv/` listo para usar**. Compilas la carpeta del título `PPSA99193` en tu PC a partir del
compilador de la versión, que no contiene nada del juego, y de tu propio volcado:

1. Descarga `mkw-pit-stop-<version>-ps5-builder-linux-x86_64.tar.zst` de las
   [**versiones**](https://github.com/exploitizm/mkw-pit-stop/releases), comprueba su suma con `SHA256SUMS` y
   descomprímelo en Linux o WSL2:
   ```sh
   tar --zstd -xf mkw-pit-stop-*-ps5-builder-linux-x86_64.tar.zst
   cd mkw-pit-stop-*
   ```
2. Ejecuta `prepare_ps5.py` apuntando a tu volcado de *Mario Kart World* (una carpeta o un archivo `.nup`):
   ```sh
   python3 prepare_ps5.py --source /path/to/your/mkw-dump --out ./PPSA99193
   ```
   El script detecta la versión de tu volcado (1.8.0 o 1.0.0), traduce el ExeFS AArch64 a C (`ps5-high144`), compila y
   enlaza `eboot.bin` con el runtime precompilado y el archivo RADV para PS5, descarga `nvdisasm` y `ptxas` de NVIDIA
   (fijados por SHA-256) para generar localmente la especificación de instrucciones SM86, traduce los 11.561 shaders a
   `PPSA99193/spirv/` y coloca `exefs/` y `romfs/` en `PPSA99193/`.
3. Transfiere `PPSA99193` a `/data/homebrew/PPSA99193` en tu PS5 por FTP o USB — o pasa `--deploy <PS5_IP>` para
   subirlo, comprobar cada archivo por SHA-256 y renovar el `param.json` y el `icon0.png` que registró ShadowMountPlus
   en `/user/app/PPSA99193/sce_sys`.
4. Abre **Mario Kart World: Pit Stop** desde la pantalla de inicio de la PS5.

**Actualizar:** descomprime el compilador más reciente y ejecuta `prepare_ps5.py` con las mismas carpetas `--work` y
`--out`. Los pasos cuyas entradas no cambiaron (como `romfs/` y los shaders sin cambios) se omiten, y tus partidas
guardadas (`profile/`) y ajustes (`mkw.ini`) en la consola se conservan.

## Superposición de ajustes Pit Stop

Pulsa **Options + clic del panel táctil** en el DualSense (o **F1** en escritorio) desde cualquier pantalla para abrir
**Pit Stop** ([docs/ps5/pit-stop.md](../../docs/ps5/pit-stop.md)):

| Control | Acción |
|---|---|
| **Options + clic del panel táctil** | Abrir o cerrar Pit Stop desde cualquier pantalla |
| **Cruceta / stick izquierdo arriba y abajo** | Moverse una fila |
| **Cruceta / stick izquierdo izquierda y derecha** | Cambiar el ajuste seleccionado (No/Sí o un valor por pasos) |
| **Círculo / Cruz** | Confirmar / Atrás (sigue tu disposición de botones Nintendo o Xbox) |
| **L1 / R1** | Categoría anterior / siguiente dentro de **All settings** |

- **Las filas rápidas** de la primera pantalla ajustan en vivo **Master volume** (0–100 %), **Vibration strength**
  (0–300 %) y **Performance HUD** (Off, FPS, Full).
- **All settings** abre **Display**, **Graphics**, **Controls** (con captura de botones por acción en vivo), **Audio**,
  **General**, **Advanced**, **Hacks** (desbloquear todos los pilotos, vehículos, pegatinas y la Copa Especial) y
  **Multiplayer**. Los cambios se guardan solos en `/app0/mkw.ini`.

## Jugar en PC (Linux y Windows)

Del proyecto upstream [mkw-recomp](https://github.com/adriadam10/mkw-recomp):

<p align="center">
  <img src="../../docs/images/launcher-files.png" width="400" alt="La página Game files de la pantalla de inicio antes de la preparación">
  <img src="../../docs/images/launcher-main.png" width="400" alt="La pantalla de inicio con el juego ya preparado">
</p>

1. Vuelca Mario Kart World 1.0.0 o 1.8.0 de tu Switch 2 (`exefs/` y `romfs/`, descifrados).
2. Descarga el lanzador de PC de las [versiones de mkw-recomp](https://github.com/adriadam10/mkw-recomp/releases): en
   Linux el `.AppImage` (guarda el juego que compila en `~/.local/share/mkw-recomp`; glibc 2.31 o posterior), en Windows
   el `.zip`. Ambos necesitan unos 30 GB libres.
3. Ejecuta el AppImage o `mkw.exe`, abre **Game files**, indica tu volcado y pulsa **Prepare**, luego **Play**.

## Compilar desde el código fuente

### Destino nativo de PS5 (`PPSA99193`)

Consulta [docs/ps5/ps5.md](../../docs/ps5/ps5.md) y [tools/ps5/README.md](../../tools/ps5/README.md) para las copias fijadas de
`PS5_VulkanTemplate`, `PS5_Vulkan`, `PS5_Mesa` y `PS5_PayloadSDK`:

```sh
python3 recomp/gen.py --exefs "$MKW_GAME/exefs" --guest-layout ps5-high144 \
    --out build/ps5-game/generated --patches recomp/patches-mkw.txt
python3 tools/shaders/extract.py && python3 tools/shaders/translate.py
python3 tools/ps5/build_game.py --stack "$stack" --include "$headers" \
    --generated build/ps5-game/generated --exefs "$MKW_GAME/exefs"
python3 tools/ps5/deploy.py <PS5_IP> --game
```

Para empaquetar una versión del compilador para PS5 sin nada del juego a partir del inventario del runtime
(`--inventory`):

```sh
python3 tools/ps5/build_game.py --stack "$stack" --include "$headers" --inventory
python3 tools/ps5/release_ps5.py --stack "$stack" --version 0.1.0
```

### Compilaciones para Linux y Windows

Detalles en [tools/build/README.md](../../tools/build/README.md):

```sh
python3 recomp/gen.py --exefs game/exefs --out build/generated --jobs 8 --patches recomp/patches-mkw.txt
python3 tools/shaders/extract.py && python3 tools/shaders/translate.py
python3 tools/build/build.py              # build/mkw; --target windows builds mkw.exe with llvm-mingw
build/mkw
```

| Carpeta | Contenido |
|---|---|
| [recomp/](../../recomp/README.md) | El generador: AArch64 → C |
| [runtime/](../../runtime/README.md) | Lo que hace de consola: kernel, memoria, sustitutos del SDK, NVN2 sobre Vulkan, audio, entrada, Pit Stop y lanzador |
| [sm86/](../../sm86/README.md) | El traductor de shaders: SASS SM86 → GLSL → SPIR-V |
| [tools/](../../tools/README.md) | Análisis, preparación, compilación, empaquetado para PS5, ejecuciones del juego y diagnóstico |
| [tests/](../../tests/README.md) | Pruebas sin el juego ni GPU, en la CI de Linux y Windows |
| [docs/](../../docs/README.md) | Arquitectura de PS5, Pit Stop, investigación, fuentes, hoja de ruta y rendimiento |
| [i18n/](../../i18n/README.md) | Los textos del menú como archivos gettext (`mkw.pot`, un `.po` por idioma) |
| [server/](../../server/README.md) | El servidor de salas para jugar en inalámbrico local por internet (`server/lobby`) |
| `.github/` | CI, plantillas de issues y pull requests |

En la raíz: `pyproject.toml` y `uv.lock` (el entorno Python de las herramientas), `Makefile` (atajos para las versiones
y la CI), `.gitignore`, `LICENSE`, `CONTRIBUTING.md`, `CHANGELOG.md` (los cambios de cada versión, sus notas),
`AGENTS.md` (una guía para agentes de programación).

## Rendimiento

En una PS5 estándar, Mario Kart World 1.8.0, un benchmark de carrera de Gran Premio (`tools/ps5/bench.py`, 210 s de
carrera, 1080p, generación de fotogramas desactivada):

| Fecha | Tasa de fotogramas | Tiempo medio por fotograma | Fotograma típico | 1 % más bajo | Velocidad de carrera |
|---|---|---|---|---|---|
| 20/11/2026 | 60 fps (el nuevo valor por defecto) | 17,0–17,4 ms (57–59 fps) | 16,7 ms (en el límite) | 35–41 fps | 0,95–0,98x |
| 16/11/2026 | 30 fps (el valor por defecto anterior) | 33,3 ms | 33,3 ms | | 1,00x |

El nuevo valor por defecto no se ha probado en la consola tal como se distribuye: estas cifras salen de carreras de
benchmark con el registro de rendimiento del runtime activado (`MKW_TRACE=perf`, que cuesta un poco), puestas a 60 fps a
mano. Los menús, las contrarrelojes, la pantalla dividida, las sesiones largas y otros circuitos aún no se han jugado con
el nuevo valor por defecto.

Alrededor del 2 % de los fotogramas de carrera tarda más de 20 ms: es donde un circuito carga texturas nuevas, y marcan
el 1 % más bajo y la ligera cámara lenta. Lo medido y lo pendiente está en
[docs/project/roadmap.md](../../docs/project/roadmap.md) y [docs/ps5/ps5.md](../../docs/ps5/ps5.md#frame-rate); las cifras de PC, en
[docs/project/performance.md](../../docs/project/performance.md).

## Informar de errores

Consulta [CONTRIBUTING.md](../../CONTRIBUTING.md). Informa de los problemas de PS5 en las
[issues de este fork](https://github.com/exploitizm/mkw-pit-stop/issues) y adjunta el registro del kernel de la consola
(`klog`, con el prefijo `[Mario Kart World: Pit Stop]`) y `logs/setup.log` del compilador de PC. Para problemas de
escritorio, adjunta `logs/mkw.log` y `logs/setup.log`. No adjuntes nunca archivos del juego, volcados ni binarios
`eboot.bin` compilados a partir de tu volcado.

## Licencias, créditos y aviso legal

El código fuente de este repositorio está bajo la [licencia MIT](../../LICENSE) (`Copyright (c) 2026 Adrià`).

**Los archivos de las versiones para PS5 son GPL-3.0-or-later.** Cada título de PS5 enlaza la capa de plataforma del
fork del SDK de payloads y el código de arranque de `PS5_Vulkan` (GPL-3.0-or-later), así que el título provisional, el
`ps5-bundle/` del compilador y cada título que compiles con ellos están bajo GPL-3.0-or-later en conjunto; el código
propio de mkw-recomp sigue siendo MIT. Cada versión lleva `LEGAL.txt` y `licenses/` (cada parte con su licencia,
copyright y revisión de origen) y adjunta el código fuente completo de cada parte como `source/*.tar.gz`
(`source/SOURCES.txt`), generados por [tools/ps5/release_notices.py](../../tools/ps5/release_notices.py).

- **Proyecto upstream:** [**mkw-recomp**](https://github.com/adriadam10/mkw-recomp) de Adrià — el traductor AArch64→C,
  el kernel de Switch 2 y los sustitutos del SDK `nn::*`, el renderizador NVN2 sobre Vulkan y el traductor de shaders
  SM86→SPIR-V sin conexión.
- **Proyectos MIT y permisivos** (archivos `LICENSE.*` en [recomp/](../../recomp/), [runtime/](../../runtime/),
  [sm86/decode/](../../sm86/decode/) y [runtime/third_party/](../../runtime/third_party/README.md)):
  [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) y su port a Linux,
  [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp),
  [DocumentSASS](https://github.com/0xD0GF00D/DocumentSASS) y
  [nvsass-disassembler](https://github.com/reoLantern/nvsass-disassembler),
  [Ryujinx](https://git.ryujinx.app/ryubing/ryujinx), [Mesa NAK](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/nouveau/compiler),
  [mario-kart-world-save-editor](https://github.com/Ca1nM/mario-kart-world-save-editor), Dear ImGui,
  [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), AMD FidelityFX FSR 1, Vulkan Memory Allocator, Roboto y
  Lato.
- **Herramientas y runtime de PS5:** [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate) (MIT, Sascha
  Willems y Mihawk), [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) (MIT, RADV para PS5),
  [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) y [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK)
  (GPL-3.0-or-later, Mihawk y [John Törnblom / ps5-payload-dev](https://github.com/ps5-payload-dev/sdk)), y
  [ShadowMountPlus](https://github.com/drakmor/shadowMountPlus). Enlazado en un título de PS5 con la capa de plataforma
  del SDK de payloads y la CRT y los stubs de `PS5_Vulkan`, el binario combinado del título de PS5 queda bajo
  GPL-3.0-or-later (`LEGAL.txt` y `licenses/` dentro del título compilado).
- **Solo investigación e ideas de diseño** (no se copió código de proyectos GPL o sin licencia): eden-nx2, Hookshot,
  PS5_Hookshot, EglantinePS5, AnyPS5, prosper y shadPS4. Cada fuente está documentada en
  [docs/project/research.md](../../docs/project/research.md#sources).

**Aviso legal:** *Mario Kart World* y *Mario* son marcas comerciales de Nintendo. «PlayStation» y «PS5» son marcas
comerciales de Sony Interactive Entertainment Inc. Este proyecto no está afiliado a Nintendo, Sony Interactive
Entertainment ni ningún editor de juegos, ni cuenta con su respaldo, no contiene código, shaders ni recursos del juego y
no ayuda a obtenerlos.
