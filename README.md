# Swordigo — Nintendo Switch port

A wrapper port of the Android version of **Swordigo** (v1.4.12) for the
Nintendo Switch. It loads the original arm64 game binary, resolves its imports
with native Switch libraries and runs it.

Based on the original [Swordigo Vita](https://github.com/) port by TheFloW and
Rinnegatamante, and [max_nx](https://github.com/fgsfdsfgs/max_nx) AArch64
Switch wrapper by fgsfds.

## Install

You need the **Swordigo 1.4.12** `.apk`

1. Make a folder for the game on your SD card, e.g. `/switch/swordigo/`.
2. From the APK, copy into that folder:
   - `lib/arm64-v8a/libswordigo.so`
   - the **`assets/`** folder (so you have `.../swordigo/assets/resources/...`)
   - the **`res/`** folder (the `.mp3` music, so `.../swordigo/res/*.mp3`)
3. Copy `swordigo_nx.nro` into the same folder.

Final layout:

```
/switch/swordigo/swordigo_nx.nro
/switch/swordigo/libswordigo.so
/switch/swordigo/assets/resources/...
/switch/swordigo/res/*.mp3
```

Launch via a **game override** (hold R on an installed title) or a forwarder

## Known Issue

- The game over screen locks up the game, requiring you to restart the app.

## Build

Install devkitA64 and the portlibs:

```
pacman -S switch-dev switch-mesa switch-libdrm_nouveau \
          switch-openal switch-mpg123 switch-sdl2 switch-zlib
```

## Credits

- **TheFloW** — original Swordigo `.so` loader.
- **Rinnegatamante** — Vita port, audio and JNI work.
- **fgsfds** — `max_nx`, the Switch AArch64 wrapper framework.

## Support

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

## Legal

Not affiliated with Touchfoo / Ville Mäkynen. "Swordigo" is the property of its
owner. No assets or original program code are included; obtain the game
legally. The wrapper source here is MIT licensed (see `LICENSE`).
