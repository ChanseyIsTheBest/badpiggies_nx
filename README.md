# Bad Piggies — Nintendo Switch port (Unity / IL2CPP wrapper)
 
This is a native wrapper / loader that runs the original ARM64 build of Bad Piggies on Switch homebrew. It contains no game code and no game assets

## Install & run
 
You need files from "Bad Piggies 2.4.3297" (`com.rovio.BadPiggies`).
 
Copy the `.nro` to your SD card (e.g. `sdmc:/switch/badpiggies_nx/badpiggies_nx.nro`), then place your game files next to the `.nro`, in the same folder:

```
sdmc:/switch/badpiggies_nx
├── badpiggies_nx.nro
├── libmain.so                     <- from your APK: lib/arm64-v8a/
├── libunity.so                    <- from your APK: lib/arm64-v8a/
├── libil2cpp.so                   <- from your APK: lib/arm64-v8a/
└── assets/                        <- from your APK: the whole assets/ folder
```

Optionally, drop a `cursor.png` (64x64, transparency supported) in the same folder to replace the on-screen cursor with your own.

## Controls
 
| Input | Action |
| --- | --- |
| `+` | Toggle the on-screen cursor |
| `-` | Toggle gyro pointing (tilt/turn the controller to aim) |
| Left stick | Move the cursor |
| `L` / `R` | Recenter the cursor to the middle of the screen (helps gyro aiming) |
| `A` / `ZR` / `ZL` | Tap / confirm (ZL and ZR let you play one-handed) |
| `B` | Back button
| D-pad up / down | Adjust sensitivity of whatever is driving the cursor |

A USB mouse works in both handheld and docked: move to control the cursor, left-click to tap, and use the scroll wheel to change 
sensitivity.
Your stick, mouse and gyro sensitivities are remembered in `pointer.cfg` automatically after in-game adjustment.

## Languages
On first launch the wrapper writes `sdmc:/switch/angrybirds/config.txt`
(one `name value` per line, `#` for comments):
 
```
# language: 'auto' follows the Switch system language, or one of the codes in
# the table below (e.g. fr, de, es, ja, zh_CN).
```

| `language=` | Game locale | UI text |
|---|---|---|
| `auto` | follows the Switch system language | — |
| `en` | en_US | English |
| `fr` | fr_FR | French |
| `it` | it_IT | Italian |
| `de` | de_DE | German |
| `es` | es_ES | Spanish |
| `ja` | ja_JP | Japanese |
| `zh_CN` | zh_CN | Simplified Chinese |

## Requirements (to build)
 
Install devkitPro with the Switch toolchain and these packages:
 
```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-freetype switch-libpng switch-zlib switch-bzip2
```


## Credits
 
The loader/shim infrastructure (`so_util`, `libc_shim`, `imports`, `error`) derives from the SoLoader lineage — TheOfficialFloW's Vita/Switch loader tradition, by way of the open-source `colorsheep_nx` and `laytonbmr_nx` Switch ports, all MIT-licensed. The stick-driven cursor and its GL overlay are ported from `laytonbmr_nx`. The Unity/IL2CPP-specific JNI, the engine patches, the `UnityEngine.Input` hooks, audio, and main loop in this project are new. Thanks to everyone in that lineage for making this approach possible.
