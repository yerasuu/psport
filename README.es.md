# psport — juegos de PS4 nativos en Linux

> El README en inglés ([README.md](README.md)) es la referencia; esta traducción puede no estar al día. Documentación técnica: [docs/en/](docs/en/ARCHITECTURE.md).

[English](README.md) · [Русский](README.ru.md) · **Español**

psport es un proyecto independiente derivado de [bbport](https://github.com/deadinside28/bloodborne_pc),
el port nativo de Bloodborne para Linux, y de [shadPS4](https://github.com/shadps4-emu/shadPS4),
llevado más allá de un solo juego. El ejecutable original de
un juego de PS4 se ejecuta en un PC Linux x86-64 igual que un programa de Windows bajo Wine: es una
capa de compatibilidad, no un emulador.

- **CPU.** La CPU de PS4 es x86-64, así que el código del juego se ejecuta tal cual en la CPU del
  PC, sin emulación ni traducción de instrucciones. El eboot se convierte de antemano (`scripts/`)
  en una imagen de memoria plana con la libc del juego enlazada; un cargador en C (`src/main/main.c`) la
  mapea y salta a ella.
- **Bibliotecas del sistema.** Las proporciona el propio port: el runtime de `src/*/runtime_*.c`
  (memoria, hilos, sincronización, archivos, audio, mando, partidas guardadas, excepciones, hilos de
  usuario, módulos que el juego carga mientras se ejecuta) y las bibliotecas de `gpu/` (libSceNgs2,
  libSceAudiodec, libSceFiber, libSceFont). Ningún juego probado necesita un volcado de los módulos
  del sistema de la consola.
- **Gráficos.** El flujo de comandos de la GPU y los shaders GCN se traducen a Vulkan con un
  renderizador derivado del núcleo de vídeo y del recompilador de shaders de
  [shadPS4](https://github.com/shadps4-emu/shadPS4) (`gpu/shadps4/`), tal como lo amplió bbport.

Bloodborne sigue funcionando como en bbport, con sus parches, reescaladores y ajustes.
Los demás juegos se arrancan con `PS_SKIP_GAME_CHECK=1`. Qué toma psport de otros proyectos (commit
de origen y licencia) y qué cambia en el código heredado: [FORK.md](FORK.md). bbport y shadPS4 se
revisan de vez en cuando; los cambios que le sirven a psport se portan a mano y se anotan en
[FORK.md](FORK.md), sin fusionar versiones enteras.

> **No se incluye ningún archivo de juego.** Necesitas tus propios volcados de los juegos que tengas.
> Este proyecto no está afiliado a Sony Interactive Entertainment, FromSoftware, AMD ni a las
> editoras de los juegos de abajo, y no tiene relación con shadPS4.

## Juegos

Probados sin ventana con `tools/regress.sh` (ver más abajo): seis de siete llegan a jugarse con
sonido.

| Juego | Title ID | Estado |
|---|---|---|
| Bloodborne (1.09) | CUSA03173 | jugable: partidas guardadas, varias zonas, sonido, mando |
| Atelier Ryza | CUSA16458 | menú del título, prólogo, el mapa con el jugador al mando, sonido |
| Atelier Ayesha DX | CUSA16921 | película de introducción, menú del título, escenas iniciales, mapa, taller, sonido |
| Tales of Symphonia Remastered | CUSA50989 | pantalla de título, la primera sala, sonido |
| Astrea | CUSA49515 | los paneles de la historia de la introducción, sonido |
| SaGa Emerald Beyond | CUSA43027 | pantalla de título, primeras escenas de la historia con diálogos, sonido |
| ALTDEUS: Beyond Chronos | CUSA26563 | pantalla en negro: exige un casco PS VR y no tiene modo de pantalla plana |

Los juegos que exigen PS VR (`require_ps_vr` en `param.sfo`) no son compatibles. Solo se ha probado a
fondo en una máquina (AMD Radeon RX 7800 XT, Mesa/RADV).

## Uso

Lo normal es usarlo a través de decanter: su sección de PS clona psport, lo compila con `build.sh`, lo
recompila cuando el checkout avanza y ejecuta cada volcado con `run.sh` (los juegos que no son
Bloodborne, con `PS_SKIP_GAME_CHECK=1 PS_PERMISSIVE=1`).

Por separado:

```bash
git clone --recursive https://github.com/yerasuu/psport.git && cd psport
bash build.sh                                   # out/psport y out/gpu/libpsport-gpu.so
PS_GAME_DIR=/ruta/a/CUSA03173 bash run.sh       # Bloodborne
PS_GAME_DIR=/ruta/al/volcado PS_SKIP_GAME_CHECK=1 PS_PERMISSIVE=1 bash run.sh
```

`PS_GAME_DIR` vale por defecto el único volcado junto al checkout (`../CUSA03173`, por
ejemplo). `PS_PERMISSIVE=1` hace que las importaciones que le faltan al port devuelvan 0 en lugar de detener el juego. `PS_SYS_MODULES=<carpeta>` enlaza, de forma
opcional, tu propio volcado de módulos del sistema (`*.sprx`) donde el port aún no tiene la
biblioteca; nada de ese volcado se distribuye. Las partidas guardadas y la caché de shaders van a
`user/`, y los ajustes a `psport.ini`. El lanzador GTK4 (`bash launcher/psport-launcher.sh`;
`launcher/install-desktop.sh` lo añade al menú de aplicaciones) es el de bbport y está pensado para
Bloodborne.

## Requisitos y compilación

- Linux x86-64 y una GPU con Vulkan 1.3.
- GCC (u otro compilador mediante `CC`), CMake, Ninja, pkg-config, Python 3 y los paquetes de
  `shell.nix`: Vulkan, SDL3, FreeType, FFmpeg, Boost, glslang y el resto de dependencias del núcleo
  de vídeo de shadPS4, además de protobuf, nlohmann_json, OpenSSL, zlib y miniupnpc para el módulo
  en línea opcional. Si faltan y [Nix](https://nixos.org) está instalado, `build.sh` y `run.sh`
  entran solos en `shell.nix`.
- Submódulo (Dear ImGui): clona con `--recursive`, o deja que `build.sh` ejecute
  `git submodule update --init --recursive`.
- `PS_LTO=OFF` desactiva la optimización en el enlazado; `PS_PGO=generate|use|off` controla la
  optimización guiada por perfil (`use` por defecto cuando `pgo/` contiene un perfil).
- Con [ccache](https://ccache.dev) instalado (`shell.nix` lo incluye), la biblioteca de la GPU se
  compila a través de él, con una sola caché para todos los checkouts y worktrees
  (`~/.cache/psport/ccache`, `CCACHE_DIR`).
- `nix-build packaging/port.nix` compila el port desde el código en el sandbox de Nix; la CI compila
  cada commit de `master` y publica la build de cada tag `vX.Y.Z` como caché binaria de Nix y como
  GitHub Release, con un paquete que funciona sin Nix (`psport-<tag>-x86_64.tar.zst`,
  `tools/bundle.sh`; FORK.md, "CI and binary cache").

## Medir el progreso

`tools/regress.sh` ejecuta cada juego de `tools/regress/titles.txt` dentro de un gamescope sin
ventana (`PS_AUDIO=none`, `PS_PAD_HOST=0`: sin ventana, sin sonido y sin los mandos del usuario), lo
maneja con un guion de pulsaciones (`tools/regress/*.sched`) e informa de hasta dónde ha llegado
(cargado, entrada en el código del juego, inicializado, primer fotograma de la GPU, fotogramas
dibujados, audio), de su firma de fallo y de cuántas importaciones sin implementar ha llamado; con
`-c <results.json anterior>` señala las regresiones. Los siete se ejecutan sin volcado de módulos
del sistema; `SYS_MODULES=<carpeta>` pasa uno, de forma opcional, a los juegos que no son
Bloodborne, para compararlos con las bibliotecas propias del port. Uso e hitos:
[FORK.md](FORK.md#regression-runs).

## Diagnóstico

Antes de arrancar el juego se listan todas las importaciones sin implementar o atendidas por un
stub. Las variables de diagnóstico (`PS_PERMISSIVE_OUT`, `PS_AUDIO_STATS`, `PS_NGS2_TRACE`,
`PS_FIBER_TRACE`) y los interruptores de módulos del sistema (`PS_SYS_MODULES`, `PS_HLE`, `PS_LLE`,
`PS_SYS_FONTS`) se describen en [FORK.md](FORK.md). `tools/gcn_dis.sh` desensambla shaders GCN.

## Reglas

Según [CLAUDE.md](CLAUDE.md): nada de apaños para un juego o un shader concretos, los arreglos
corrigen el comportamiento de fondo; no hay tests en el repositorio (los cambios se comprueban con
`tools/regress.sh`; las pruebas en hardware, cuando hacen falta, son homebrew de OpenOrbis); hay que
seguir las convenciones del código que rodea al cambio.

## Estructura del repositorio

| Ruta | Contenido |
|---|---|
| `src/` | Cargador (`src/main/`) y runtime (`runtime_*.c`) |
| `scripts/` | `online.py`, las direcciones del juego en línea que `run.sh` escribe con `PS_ONLINE=1` (el único paso del arranque en Python), y `sce_file.py` para las herramientas en Python |
| `gpu/` | Biblioteca del renderizador: núcleo de vídeo de shadPS4 con los cambios de bbport (`gpu/VENDOR.txt`), bibliotecas portadas (`ngs2/`, `audiodec/`, `fiber/`, `font/`), overlay y menú del juego (`port/`), puente de DLSS, módulo en línea |
| `launcher/`, `packaging/` | El lanzador GTK4 de bbport; paquete de Nix y script de AppImage |
| `patches/` | Parches de la comunidad para Bloodborne |
| `tools/` | Ejecuciones de regresión, desensamblador GCN, herramientas de FSR 4 y `fsr4cap`, utilidades para ejecuciones guionizadas |
| `docs/` | Una carpeta por idioma: `docs/en/` (arquitectura, [cómo añadir un título](docs/en/ADDING_A_TITLE.md), notas de funciones, historia de bbport en inglés) y `docs/ru/` (originales rusos de bbport, mismos nombres) |

## Créditos y licencias

psport se distribuye bajo la **GNU GPL v2 o posterior** ([LICENSE](LICENSE)).

### Proyectos en los que se basa

| Proyecto | Qué aporta | Licencia |
|---|---|---|
| [bbport](https://github.com/deadinside28/bloodborne_pc), de deadinside | el origen de psport: cargador nativo, runtime HLE, renderizador Vulkan, todo lo de Bloodborne | GPL-2.0-or-later |
| [shadPS4](https://github.com/shadps4-emu/shadPS4) | núcleo de vídeo, recompilador de shaders, AvPlayer y otras bibliotecas que incluye bbport; origen de los ports de libSceFiber y libSceFont | GPL-2.0-or-later |
| [AnyPS5](https://github.com/boykopovar/AnyPS5) | origen de los ports de libSceNgs2 y libSceAudiodec | GPL-2.0 |

El propio psport lo mantiene Yerasuu para [decanter](#uso). Qué cambia psport en el código heredado
y qué porta (con los commits de origen), junto con los últimos puntos revisados de bbport y shadPS4,
queda registrado en [FORK.md](FORK.md).

### Código de terceros incluido

| Componente | Para qué | Licencia |
|---|---|---|
| [sirit](https://github.com/shadps4-emu/sirit) | generación de SPIR-V | BSD-3-Clause |
| Cabeceras GCN de AMD (`gpu/third_party/gcn`) | decodificación de instrucciones GCN | MIT |
| [half](https://half.sourceforge.net/) | números de media precisión | MIT |
| [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan), de FireBurn, y AMD FidelityFX SDK | FSR 3.1 | MIT |
| [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) | decodificación de audio ATRAC9 | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | overlay y menús del juego | MIT |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | módulo en línea | MIT |
| Puente de DLSS (`gpu/dlss_bridge`) del [port para Windows de Supermedo](https://github.com/Supermedo/bloodborne_pc), adaptado de [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR) | DLSS | MIT |
| [Fuentes DejaVu](https://dejavu-fonts.github.io/) | texto del overlay, fuente de reserva de libSceFont | Bitstream Vera (los cambios de DejaVu son de dominio público) |
| [dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) | crear los archivos de FSR 4.1.1 en el equipo del usuario | MIT |

Se enlazan desde el sistema o desde el entorno de Nix, sin incluirse, cada uno con su licencia:
FFmpeg, FreeType, Vulkan, SDL3, Boost, fmt y el resto de lo que aparece en `shell.nix`.

### Comunidad

Parches de Bloodborne de Kyo, Lance McDonald, auser1337, illusion, emoose y otros miembros de la
comunidad (`patches/Bloodborne.xml`).

### No se distribuye

Ningún archivo de juego ni módulo del sistema de la consola. Tampoco se incluyen las DLL ni los datos
del modelo de FSR 4 de AMD, ni el SDK ni las bibliotecas de DLSS de NVIDIA; los aporta el usuario.
