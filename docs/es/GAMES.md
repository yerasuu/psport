# Juegos

Lo que hace cada juego con psport. El [README](../../README.es.md) solo indica el estado; esta
página tiene el detalle. Otros idiomas: [English](../en/GAMES.md), [Русский](../ru/GAMES.md).

## Estados

| Estado | Significado |
|---|---|
| jugable | Se puede jugar: los controles, las partidas guardadas (si el juego las tiene) y el sonido funcionan en todo lo visto hasta ahora. |
| experimental | Arranca y muestra menús o partida, pero solo se ha visto una parte limitada o solo la cubre una ejecución guionizada; son probables los fallos. |
| roto | Arranca, pero falla una parte esencial (una escena negra, un cierre inesperado). |
| no soportado | No puede funcionar con psport tal como es (por ejemplo, necesita un hardware que psport no ofrece). |

El estado es hasta dónde se ha visto llegar al juego, no una partida completa. Todos los juegos
figuran en [tools/regress/titles.txt](../../tools/regress/titles.txt) y se ejecutan sin ventana con
`tools/regress.sh` (véase Pruebas en el README); «regress» significa lo que registra esa ejecución
guionizada: el primer fotograma de la GPU, los fotogramas dibujados y el sonido. El detalle se
comprobó por última vez con la regress del 22-10-2026.

## Juegos

| Juego | ID | Estado | Detalle |
|---|---|---|---|
| Bloodborne (1.09) | CUSA03173 | jugable | Partidas guardadas, varias zonas, sonido, mando, teclado y ratón (las indicaciones de teclas se dibujan sobre las del mando), ajustes de fotogramas 30, 60, 90 y sin límite, salida a 3840x2160 (unos 80 FPS en la escena de prueba con una RX 7900 XT). El juego en línea mediante los servidores comunitarios del título (`PS_ONLINE=1`) funciona, con tirones ocasionales al interactuar con mensajes. Las correcciones por título se declaran en [QUIRKS.md](../en/QUIRKS.md). |
| Atelier Ryza | CUSA16458 | experimental | Menú principal, prólogo, el mapa con el control del jugador, sonido; el río y el bosque se dibujan bien. Guardar falla: el diálogo de guardado se rechaza y no se escribe ninguna partida. |
| Atelier Ayesha DX | CUSA16921 | jugable | Vídeo de introducción, menú principal, escenas iniciales, el mapa, el taller, sonido; las indicaciones de teclas se dibujan para jugar con teclado. |
| Tales of Symphonia Remastered | CUSA50989 | experimental | Pantalla de título, la primera sala, sonido. |
| Astrea | CUSA49515 | experimental | Las viñetas de la historia de la introducción, sonido. |
| SaGa Emerald Beyond | CUSA43027 | experimental | Pantalla de título, primeras escenas de la historia con diálogos, sonido. |
| ALTDEUS: Beyond Chronos | CUSA26563 | no soportado | Pantalla negra: requiere un casco PS VR y no tiene modo de pantalla plana. |
| Gal Guardians: Demon Purge | CUSA41240 | experimental | Regress: fotogramas dibujados, sonido. |
| Gal Guardians: Servants of the Dark (JP) | CUSA52187 | experimental | Regress: fotogramas dibujados, sonido. |
| Call of Duty: Black Ops | CUSA57547 | experimental | Regress: fotogramas dibujados, sonido. Teclado y ratón con la cámara del propio juego, una distribución de shooter y una mira. |
| Call of Duty: Black Ops II | CUSA57548 | jugable | Primera misión (Pyrrhic Victory) por puntos de control encadenados hasta la barcaza del río, Guardar y salir, Reanudar historia, vídeos, sonido, mando. |
| Blood Omen 2 | CUSA49769 | experimental | Regress: fotogramas dibujados, sin sonido. |
| Onigiri | CUSA01458 | experimental | Regress: fotogramas dibujados, sonido. |
| OCTOPATH TRAVELER 0 (JP) | CUSA53477 | roto | Carga una partida guardada y dibuja fotogramas y sonido, pero la escena 3D sigue negra (solo se ve el minimapa). Fuera de las ejecuciones habituales. |

## Notas

- Los juegos que requieren PS VR (`require_ps_vr` en `param.sfo`) no son compatibles.
- Ningún juego probado necesita un volcado de los módulos del sistema de la consola.
