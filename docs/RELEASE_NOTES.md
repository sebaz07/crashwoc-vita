# v0.1.0-beta

First public beta of the PS Vita port. / Primera beta pública del port para PS Vita.

## Files / Archivos

| File | What it is |
|---|---|
| `crashwoc.vpk` | The game for the Vita (install with VitaShell) / El juego para la Vita |
| `crashwoc-extract.exe` | Windows: copies the game files from **your own** USA disc image / copia los archivos de **tu propio** disco USA |
| `crashwoc-extract-linux`, `crashwoc-extract-macos` | Same for Linux / macOS (Apple Silicon) |
| `SHA256SUMS.txt` | Checksums |

No game files are included: you need your own GameCube disc (USA, `GCBE7D`).
No se incluyen archivos del juego: necesitas tu propio disco de GameCube (USA, `GCBE7D`).

Installation / Instalación: see the [README](../README.md) / ver el [README en español](../README.es.md).

## Highlights / Destacado

- Whole game playable with saves, music, sound and LiveArea bubble.
  Juego completo jugable con partidas, música, sonido y burbuja de LiveArea.
- Screen modes 4:3 / stretched / widescreen (Select + up) and frame lock
  (Select + down). Modos de pantalla (Select + arriba) y bloqueo de fps
  (Select + abajo).
- Render thread on a second CPU core and GPU-side vertex streaming.
  Renderizado en un segundo núcleo y vértices directo a la GPU.

## Known issues / Problemas conocidos

- Some crushing platforms in *Earth Crisis* may not show up.
  Puede que algunas plataformas aplastadoras de *Earth Crisis* no aparezcan.
- Busy scenes can drop below 30 fps; level loads take ~8–13 s.
  Las escenas cargadas pueden bajar de 30 fps; las cargas tardan ~8–13 s.

Please report problems with `ux0:data/crashwoc/crashwoc.log`.
Reporta los problemas adjuntando `ux0:data/crashwoc/crashwoc.log`.
