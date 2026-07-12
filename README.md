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

## Controls
In handheled - touch screen support
In handeld and docked
"+" brings up the cursor
"-" hides the cursor
A taps

## Requirements (to build)
 
Install devkitPro with the Switch toolchain and these packages:
 
```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib
```
 
`switch-mesa` provides GLES2/EGL; `switch-sdl2` backs the audio device.
 
```sh
export DEVKITPRO=/opt/devkitpro
make
```
 
Produces `badpiggies_nx.nro`

## Credits
 
The loader/shim infrastructure (`so_util`, `libc_shim`, `imports`, `error`) derives from the SoLoader lineage — TheOfficialFloW's Vita/Switch loader tradition, by way of the open-source `colorsheep_nx` and `laytonbmr_nx` Switch ports, all MIT-licensed. The stick-driven cursor and its GL overlay are ported from `laytonbmr_nx`. The Unity/IL2CPP-specific JNI, the engine patches, the `UnityEngine.Input` hooks, audio, and main loop in this project are new. Thanks to everyone in that lineage for making this approach possible.
