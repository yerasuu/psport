# psport

Juegos de PS4 nativos en Linux.

[English](README.md) · **Español** · [Русский](README.ru.md)

> El README en inglés ([README.md](README.md)) es la referencia; esta traducción puede ir por
> detrás. La documentación técnica está en inglés, en [docs/en/](docs/en/ARCHITECTURE.md).

## Descripción

psport es una capa de compatibilidad de PS4 para Linux x86-64. No es un emulador: funciona como
Wine con los programas de Windows. La CPU de PS4 es x86-64, así que el ejecutable original del juego
se ejecuta directamente en el procesador del PC, sin traducir instrucciones. psport carga el
ejecutable y los módulos que enlaza, proporciona las bibliotecas del sistema que importa el juego y
traduce su trabajo de GPU a Vulkan.

psport es un proyecto independiente. Nació de
[bbport](https://github.com/deadinside28/bloodborne_pc), el port nativo de Bloodborne para Linux, y
se apoya en el núcleo de vídeo y el recompilador de shaders de
[shadPS4](https://github.com/shadps4-emu/shadPS4). Se está generalizando de un solo juego a una capa
para cualquier título; Bloodborne sigue siendo compatible, con su propio perfil de juego.

> [!IMPORTANT]
> **No se incluye ningún archivo de juego.** El usuario necesita sus propios volcados de los juegos
> que posea. Este proyecto no está afiliado a Sony Interactive Entertainment, FromSoftware, AMD ni a
> las editoras de los juegos de la lista, y no tiene relación con shadPS4.

## Estado

psport está en desarrollo activo. Un juego es **jugable**, **experimental** (arranca, pero solo se
ha visto una parte limitada), **roto** (arranca pero falla una parte esencial) o **no soportado**.
El estado es hasta dónde se ha visto llegar al juego, no una partida completa; lo que hace cada
juego está en [docs/es/GAMES.md](docs/es/GAMES.md). Todos los juegos de la tabla figuran
en [tools/regress/titles.txt](tools/regress/titles.txt) y se ejecutan sin ventana con
`tools/regress.sh` (véase [Pruebas](#pruebas)).

| Juego | ID | Estado |
|---|---|---|
| Bloodborne (1.09) | CUSA03173 | jugable |
| Atelier Ryza | CUSA16458 | jugable |
| Atelier Ayesha DX | CUSA16921 | jugable |
| Tales of Symphonia Remastered | CUSA50989 | experimental |
| Astrea | CUSA49515 | experimental |
| SaGa Emerald Beyond | CUSA43027 | experimental |
| ALTDEUS: Beyond Chronos | CUSA26563 | no soportado |
| Gal Guardians: Demon Purge | CUSA41240 | experimental |
| Gal Guardians: Servants of the Dark (JP) | CUSA52187 | experimental |
| Call of Duty: Black Ops | CUSA57547 | experimental |
| Call of Duty: Black Ops II | CUSA57548 | jugable |
| Blood Omen 2 | CUSA49769 | experimental |
| Onigiri | CUSA01458 | experimental |
| OCTOPATH TRAVELER 0 (JP) | CUSA53477 | roto |

- Los juegos que requieren PS VR (`require_ps_vr` en `param.sfo`) no son compatibles.
- Ningún juego probado necesita un volcado de los módulos del sistema de la consola.
- Solo se ha probado a fondo una máquina: AMD Radeon RX 7900 XT con Mesa (RADV).

## Requisitos

- **Sistema:** Linux en x86-64. Windows es una base que aún no ejecuta ningún juego; su compilación
  de CI es un paquete de prueba con una sonda que informa de lo que ofrece el driver de la GPU
  ([guía de pruebas en Windows](docs/en/WINDOWS_TEST.md)).
- **GPU:** Vulkan 1.3 con descriptor indexing y buffer device address. La ruta de GPU es la misma
  para todos los fabricantes y elige las funciones según lo que informa el dispositivo, no según el
  fabricante. Está probada en AMD con RADV; NVIDIA e Intel aún no están verificadas
  ([guía de pruebas en NVIDIA](docs/en/NVIDIA_TEST.md)).
- **Juegos:** los volcados propios del usuario de juegos que posea (la carpeta del juego con `eboot.bin`,
  `sce_module/` y `sce_sys/param.sfo`).
- **Herramientas de compilación**, una de estas opciones:
  - [Nix](https://nixos.org): si faltan dependencias, `build.sh` y `run.sh` entran solos en
    [shell.nix](shell.nix).
  - Paquetes del sistema: GCC (u otro compilador mediante `CC`), CMake, Ninja, pkg-config y las
    bibliotecas de `shell.nix` (Vulkan, SDL3, FreeType, FFmpeg, Boost, glslang y el resto de
    dependencias del núcleo de vídeo de shadPS4; nlohmann_json, mbedTLS, zlib y miniupnpc
    para el módulo en línea opcional).
- **Python 3** solo para el juego en línea, la compilación del módulo en línea (genera los mensajes de
  shadNet), el lanzador incluido y las herramientas de desarrollo; un arranque normal no lo necesita.

## Primeros pasos

### Con decanter (recomendado)

[decanter](https://github.com/yerasuu/decanter) es la interfaz recomendada. Su sección de PS clona
psport, lo compila con `build.sh`, lo vuelve a compilar cuando cambia la copia de trabajo e inicia
cada volcado mediante `run.sh` (los juegos que no son Bloodborne, con `PS_PERMISSIVE=1`).

### Compilar desde el código fuente

```bash
git clone --recursive https://github.com/yerasuu/psport.git
cd psport
bash build.sh        # genera out/psport y out/gpu/libpsport-gpu.so
```

Sin `--recursive`, `build.sh` descarga él mismo el submódulo (Dear ImGui).

| Opción de compilación | Efecto |
|---|---|
| `PS_LTO=OFF` | desactiva la optimización en tiempo de enlazado (activa por defecto) |
| `PS_NET=OFF` | deja fuera el módulo en línea (`libpsport-net.so`), como se compilan la caché binaria pública y el paquete |
| `PS_PGO=generate\|use\|off` | optimización guiada por perfiles; `use` por defecto cuando `pgo/` contiene un perfil |
| ccache instalado | la biblioteca de GPU se compila con él, una sola caché para todas las copias de trabajo (`~/.cache/psport/ccache`, o `CCACHE_DIR`) |

`nix-build packaging/port.nix` compila el port desde el código fuente en el entorno aislado de Nix,
con el módulo en línea; `--arg withNet false` lo deja fuera.

### Ejecutar un juego

```bash
PS_GAME_DIR=/ruta/a/CUSA03173 bash run.sh                     # Bloodborne
PS_GAME_DIR=/ruta/al/volcado PS_PERMISSIVE=1 bash run.sh      # cualquier otro juego
```

`run.sh` compila el port si hace falta y luego inicia el juego.

| Variable | Efecto |
|---|---|
| `PS_GAME_DIR` | la carpeta del juego; por defecto, el único volcado junto a la copia de trabajo |
| `PS_PERMISSIVE=1` | las importaciones que psport aún no implementa devuelven 0 en vez de detener el juego |
| `PS_DATA_DIR` | carpeta con permiso de escritura para lo compilado (`out/`), las partidas guardadas y la caché de shaders (`user/`) y `psport.ini`; por defecto, la copia de trabajo |
| `PS_SYS_MODULES=<carpeta>` | enlaza, si el usuario quiere, su propio volcado de módulos del sistema (`*.sprx`) donde psport aún no tiene biblioteca; no se distribuye nada de él |

`bash run.sh --software` dibuja con Lavapipe (el controlador Vulkan por CPU de Mesa) en vez de con
la GPU.

Durante el juego, Insert o L3+R3 abre el menú superpuesto. Las partidas guardadas y la caché de
shaders van a `user/`, y los ajustes a `psport.ini`.

### Versiones precompiladas

Cada etiqueta de versión `vX.Y.Z` la compila la CI y se publica como GitHub Release con un paquete
que funciona sin Nix: `psport-<tag>-x86_64.tar.zst` y su `.sha256`, generados por
[tools/bundle.sh](tools/bundle.sh). Se extrae en cualquier sitio con permiso de escritura y se ejecuta
con los controladores Vulkan del propio sistema:

```bash
PS_DATA_DIR=<carpeta escribible> PS_GAME_DIR=/ruta/al/volcado bash <carpeta extraída>/run.sh
```

El paquete aún no lleva el módulo en línea (`libpsport-net.so`): ya no enlaza OpenSSL, abseil ni
protobuf (Apache-2.0, que no se puede distribuir junto a la GPL-2.0 de psport), sino mbedTLS con su
opción GPL-2.0-or-later, y queda fuera hasta que se compruebe el juego en línea con él; mientras
tanto, para jugar en línea (`PS_ONLINE=1`) hay que compilar desde el código fuente. Tampoco lleva el cargador de Vulkan: usa el `libvulkan.so.1` del sistema, así que no
arranca en un sistema que no lo tenga en sus carpetas de bibliotecas (NixOS no tiene ninguno en
`/run/opengl-driver/lib`).

La misma compilación, también sin el módulo en línea, se publica como caché binaria de Nix, de donde
decanter descarga sus compilaciones ya hechas; véase [FORK.md](FORK.md#ci-and-binary-cache). Una
compilación local (`build.sh`, `nix-build packaging/port.nix`) sí lleva el módulo en línea.

### Lanzador

[launcher/](launcher/) contiene el lanzador GTK4 heredado de bbport, pensado para Bloodborne:
`bash launcher/psport-launcher.sh` lo inicia y `bash launcher/install-desktop.sh` lo añade al menú
de aplicaciones.

## Configuración

Los ajustes están en `psport.ini`, en la carpeta de datos (`PS_CONFIG` indica otro archivo). La
mayoría se cambian desde el menú superpuesto, que escribe el archivo. Claves y variables más
utilizadas:

| Ajuste | Efecto |
|---|---|
| `output_res` | resolución de salida; en un juego con preajustes, los tamaños distintos de 1080p escalan la escena |
| `upscaler` (`PS_UPSCALER`) | reescalador temporal, donde lo coloca el perfil del juego ([notas del reescalador](docs/en/upscaler.md)) |
| `frame_gen` (`PS_FRAME_GEN`) | `off` (por defecto) o `fsr`: generación de fotogramas FSR 3.1, un fotograma generado por cada fotograma del juego, donde la GPU cumple sus requisitos (fila `frame_gen_fsr` de `--list-features`); `auto` y `dlss` aún no están disponibles y la dejan desactivada; se lee al arrancar ([generación de fotogramas](docs/en/upscaler.md#frame-generation)) |
| `menu_language` (`PS_MENU_LANGUAGE`) | idioma del menú superpuesto: `en`, `es` o `ru` |
| `game_mouse_sensitivity` | escala del movimiento del ratón del sistema que recibe el juego (de 0,05 a 20; 1 por defecto) |
| `PS_LANGUAGE` | el idioma del sistema que lee el juego (1 inglés, 3 español, 8 ruso) |
| `PS_FPS` | preajuste de fotogramas por segundo en los juegos que los tienen (Bloodborne: 30, 60, 90, `uncap`) |
| `PS_ONLINE=1` | juego en línea mediante los servidores comunitarios del título ([juego en línea](docs/en/ARCHITECTURE.md#online-play)) |
| `PS_MODS_ENABLED=0` | desactiva los mods de archivos ([mods y parches](docs/en/MODS.md)) |
| `PS_GAME_PROFILE=none` | ejecuta sin el perfil del juego, solo por las rutas genéricas |
| `PS_QUIRKS=none,-<id>,+<id>` | desactiva o activa las correcciones para un juego o un sistema ([quirks](docs/en/QUIRKS.md)) |
| `toypad` (`PS_TOYPAD=1`) | conecta el toy pad de LEGO Dimensions emulado, desactivado por defecto ([toy pad](docs/en/TOYPAD.md)) |
| `skip_movies` (`PS_SKIP_MOVIES=1`) | `key.skip_movie` (Escape) o `pad.skip_movie` (`misc1`) detiene los vídeos en reproducción, desactivado por defecto ([FORK.md, Keyboard](FORK.md#keyboard)) |

Los valores por defecto y por qué sirven para cualquier juego: [FORK.md, Defaults](FORK.md#defaults).
Variables de diagnóstico e informe de llamadas: [FORK.md, Diagnostics](FORK.md#diagnostics). Módulos
y fuentes del sistema (`PS_SYS_MODULES`, `PS_HLE`, `PS_LLE`, `PS_SYS_FONTS`):
[FORK.md, System modules](FORK.md#system-modules).

## Cómo funciona

- **Cargador.** Un cargador en C lee el eboot y los módulos que enlaza (SELF o ELF) directamente de
  la carpeta del juego, los reubica y los enlaza en una sola imagen y salta al código del juego.
- **Bibliotecas del sistema.** psport las implementa de forma nativa: un runtime en C (memoria,
  hilos, sincronización, archivos, audio, mando, partidas guardadas, excepciones, servicios, hilos
  de usuario, módulos cargados durante la ejecución) y las bibliotecas de `gpu/` (GnmDriver,
  VideoOut, AvPlayer, Videodec2, Ngs2, Audiodec, Fiber, Font, PngEnc, PngDec, JpegDec).
- **Gráficos.** El flujo de comandos de la GPU y los shaders GCN se traducen a Vulkan con un
  renderizador derivado del núcleo de vídeo y del recompilador de shaders de shadPS4
  (`gpu/shadps4/`), tal como lo amplió bbport.
- **Perfiles de juego.** Lo propio de un juego (hooks y optimizaciones) vive en su perfil, en
  `gpu/games/`; todos los juegos funcionan por las rutas genéricas sin él.
- **Nada en silencio.** Antes de que empiece un juego se listan todas las importaciones sin
  implementar o resueltas con un stub; las llamadas que fallan o no hacen trabajo real se registran
  y se cuentan en un informe de llamadas.

Detalles: [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md) (componentes y flujo de arranque) y
[FORK.md](FORK.md) (lo que psport toma de otros proyectos y lo que cambia).

## Desarrollo

### Pruebas

`tools/regress.sh <carpeta de juegos>` ejecuta todos los juegos de `tools/regress/titles.txt` en un
gamescope sin ventana, sin sonido y sin ninguno de los mandos del usuario. Un guion de mando
(`tools/regress/*.sched`) maneja cada juego, y el informe indica hasta dónde llegó (cargado,
iniciado, inicializado, primer fotograma de GPU, fotogramas dibujados, audio), su firma de fallo y
cuántas importaciones sin implementar llamó. Con `-c <results.json anterior>` señala las
regresiones. Uso e hitos: [FORK.md, Regression runs](FORK.md#regression-runs).

### Poner en marcha un juego

[docs/en/ADDING_A_TITLE.md](docs/en/ADDING_A_TITLE.md) explica cómo ejecutar un volcado nuevo, leer
su registro y su informe de llamadas, implementar lo que falta y comprobar el resultado.

### Normas para los cambios

- Las correcciones atacan el comportamiento de fondo; nada de apaños para un juego o un shader
  concreto en el núcleo.
- Las optimizaciones para un solo juego van en su perfil (`gpu/games/<juego>/`), y todos los juegos
  deben funcionar correctamente sin él (`PS_GAME_PROFILE=none`).
- Una corrección temporal que necesita un juego es un quirk declarado, registrado en el log y
  desactivable ([docs/en/QUIRKS.md](docs/en/QUIRKS.md)).
- Sin pruebas en el repositorio: los cambios se comprueban con `tools/regress.sh`; las pruebas en
  hardware, cuando hacen falta, son homebrew de OpenOrbis.
- Los cambios siguen el estilo del código que los rodea; la nomenclatura y el resto de convenciones están en
  [CLAUDE.md](CLAUDE.md).
- Los cambios de bbport y shadPS4 se portan a mano, cada uno anotado en [FORK.md](FORK.md) con su
  commit de origen.

`tools/gcn_dis.sh` desensambla shaders GCN.

### Estructura del repositorio

| Ruta | Contenido |
|---|---|
| `src/` | punto de entrada (`src/main/`), cargador nativo y runtime en C |
| `gpu/` | biblioteca de GPU: núcleo de vídeo de shadPS4 con los cambios de bbport ([gpu/VENDOR.txt](gpu/VENDOR.txt)), decodificador PM4 (`cp/`), módulo de memoria (`layer/`), bibliotecas portadas (`ngs2_anyps5/`, `ngs2/` su reescritura nativa, `fiber/`, `font/`, `pngenc/`, `pngdec/`, `jpegdec/`), libSceAudiodec (`audiodec/`), perfiles de juego (`games/`), menú superpuesto y menú del juego (`port/`), puente DLSS, módulo en línea (`net/`) |
| `scripts/` | `online.py` (direcciones en línea para `PS_ONLINE=1`) y `sce_file.py` para las herramientas en Python |
| `launcher/`, `packaging/` | el lanzador GTK4 de bbport; paquete de Nix y script de AppImage |
| `patches/` | parches de la comunidad para Bloodborne |
| `tools/` | ejecuciones de regresión, desensamblador GCN, herramientas de FSR 4, utilidades para ejecuciones guionizadas, paquete de versión |
| `docs/` | `docs/en/` (arquitectura, poner en marcha un juego, notas de funciones, historial de bbport en inglés), `docs/ru/` (los originales en ruso de bbport) |

### Más información

- [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md): componentes y flujo de arranque.
- [docs/en/ADDING_A_TITLE.md](docs/en/ADDING_A_TITLE.md): poner en marcha un volcado nuevo.
- [docs/en/QUIRKS.md](docs/en/QUIRKS.md): el registro de correcciones para un juego, un sistema o
  un middleware.
- [FORK.md](FORK.md): componentes portados, cambios en el código heredado, variables, informe de
  llamadas, ejecuciones de regresión, CI.
- Notas de funciones: [reescalador](docs/en/upscaler.md),
  [trabajo de GPU en paralelo](docs/en/parallel_gpu.md),
  [vectores de movimiento](docs/en/motion_vectors.md), [mods y parches](docs/en/MODS.md),
  [perfilado](docs/en/PROFILING.md), [toy pad](docs/en/TOYPAD.md).
- [docs/en/history/](docs/en/history/): notas de diseño, planes y registros de cambios de bbport
  (los originales en ruso están en [docs/ru/](docs/ru/)).

## Licencia y créditos

psport se distribuye bajo la **GNU General Public License, versión 2** ([LICENSE](LICENSE)). Los
archivos marcados como GPL-2.0-only en su cabecera solo admiten la versión 2; el resto es
GPL-2.0-or-later y se usa como versión 2. Lo mantiene Yerasuu.

### Proyectos en los que se basa

| Proyecto | Qué aporta | Licencia |
|---|---|---|
| [bbport](https://github.com/deadinside28/bloodborne_pc), de deadinside | el origen de psport: cargador nativo, runtime HLE, renderizador Vulkan, todo lo de Bloodborne | GPL-2.0-or-later |
| [shadPS4](https://github.com/shadps4-emu/shadPS4) | núcleo de vídeo, recompilador de shaders, AvPlayer y otras bibliotecas que trae bbport; origen de los ports de libSceFiber y libSceFont y de las cabeceras de libSceNgs2 | GPL-2.0-or-later |
| [AnyPS5](https://github.com/boykopovar/AnyPS5) | origen del port de libSceNgs2 | GPL-2.0 |

Los commits de origen y los cambios en el código heredado están anotados en [FORK.md](FORK.md) y
[gpu/VENDOR.txt](gpu/VENDOR.txt).

### Código de terceros incluido

| Componente | Uso | Licencia |
|---|---|---|
| [sirit](https://github.com/shadps4-emu/sirit) | emisión de SPIR-V | BSD-3-Clause |
| Cabeceras GCN de AMD (`gpu/third_party/gcn`) | decodificación de instrucciones GCN | MIT |
| [half](https://half.sourceforge.net/) | números de coma flotante de media precisión | MIT |
| [xbyak](https://github.com/herumi/xbyak) 7.43 (`gpu/third_party/xbyak`, sus cabeceras) | código x86-64 generado en tiempo de ejecución | BSD-3-Clause |
| [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan), de FireBurn, AMD FidelityFX SDK | FSR 3.1 | MIT |
| [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) | decodificación de audio ATRAC9 | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | menú superpuesto y menús del juego | MIT |
| [Tracy](https://github.com/wolfpld/tracy) (submódulo, solo se descarga con `PS_TRACY=ON`) | compilaciones para perfilado | BSD-3-Clause |
| Cabecera de la API in-application de RenderDoc (`gpu/third_party/renderdoc`) | capturas de un fotograma | MIT |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) 0.45.0 (`gpu/third_party/httplib`), sobre su backend mbedTLS | módulo en línea | MIT |
| Puente DLSS (`gpu/dlss_bridge`) del [port para Windows de Supermedo](https://github.com/Supermedo/bloodborne_pc), adaptado de [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR) | DLSS | MIT |
| [Fuentes DejaVu](https://dejavu-fonts.github.io/) | texto del menú superpuesto, alternativa para libSceFont | Bitstream Vera (cambios de DejaVu en dominio público) |
| [dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) | generar los recursos de FSR 4.1.1 en el equipo del usuario | MIT |

### Bibliotecas del paquete de la versión

El paquete de la versión lleva en `lib/` las bibliotecas que psport enlaza desde Nix (el AppImage,
en su almacén de Nix), cada una con su licencia: FFmpeg (compilado como LGPL-2.1-or-later, sin los
codificadores GPL ni los protocolos de red), SDL3, FreeType, fmt, xxHash, glibc, libstdc++ y sus
dependencias. El archivo `THIRD-PARTY-NOTICES` del paquete enumera cada una, las bibliotecas
compiladas dentro (Boost, xbyak, Zydis, miniz, Vulkan Memory Allocator...) y el código de la tabla
anterior, con su licencia y los archivos de licencia de su código fuente; `SOURCES.txt` indica dónde
está el código fuente de cada componente GPL, LGPL y MPL, con una oferta escrita del código fuente
válida durante tres años. Los dos los genera `packaging/licences.nix`. Una compilación desde el
código fuente enlaza las mismas bibliotecas desde el sistema o el entorno de Nix (`shell.nix`).

### Comunidad

Parches de Bloodborne de Kyo, Lance McDonald, auser1337, illusion, emoose y otros miembros de la
comunidad (`patches/Bloodborne.xml`).

### Lo que no se distribuye

Ningún archivo de juego ni módulo del sistema de la consola. Tampoco se incluyen las DLL y los datos
del modelo de FSR 4 de AMD ni el SDK y las bibliotecas de DLSS de NVIDIA; los aporta el usuario. Los
recursos de FSR 4 v07 no van en el paquete ni en el AppImage: `tools/fetch_fsr4_assets.sh`, que
llevan el clon, el paquete y el AppImage, los descarga en tu equipo en `fsr4_shaders/` junto a
`run.sh` (`bash <carpeta extraída>/tools/fetch_fsr4_assets.sh` para el paquete), o con
`PS_FSR4_DIR=<carpeta de datos>/fsr4_shaders` en la carpeta de datos, donde los buscan `run.sh` y el
lanzador; el lanzador del AppImage muestra esa orden para su propia copia.
