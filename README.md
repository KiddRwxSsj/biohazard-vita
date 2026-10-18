<p align="center">
  <img src="screenshots/startup.png" alt="Resident Evil" width="560"/>
</p>

A PS Vita port of the Resident Evil 1 PC decompilation. It's the 1997 PC game's own code, rebuilt
from the [decomp](https://github.com/ecruells/resident-evil-pc-decomp) with a new platform layer
for the Vita: vitaGL for the renderer, sceCtrl and the touch panel for input, SceAudio for sound,
ffmpeg and SceAvPlayer for the movies.

> [!IMPORTANT]
> **No game files are included here or in the release.** You need your own copy of the PC
> version (the GOG release works, it's the same build as the 1997 disc).
>
> The Japanese MediaKite release (Biohazard) can sit next to it as `JPN/`, and the Director's
> Cut mode needs a `DC/` folder made with the asset migrator from the upstream project.

## Download & Install

Grab `residentevil.vpk` from the [Releases](../../releases) page and install it with VitaShell.

The shader compiler is needed at `ur0:data/libshacccg.suprx`. If you don't have it yet, extract
it with [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D).

Then the game files. Copy the folder of your PC install that has `Stage1`, `Movie`, `Sound` and
the rest in it to the memory card and call it `USA`, like this:

```
ux0:data/RE1/
├── USA/
│   ├── Data/
│   ├── Movie/
│   ├── Stage1/ ... Stage7/
│   └── ...
├── JPN/          (optional)
├── DC/           (optional)
└── mods/         (optional, see below)
```

`config.ini` and the `SAVE/` folder are created there on the first boot. Your PC saves
(`savedat1.dat` to `savedat9.dat`) can go straight into `SAVE/`, they load as they are.

## Controls

<p align="center">
  <img src="screenshots/gameplay.png" alt="In game" width="700"/>
</p>

| Vita | In game |
|---|---|
| Left stick / D-Pad | move |
| Cross | action, confirm |
| Circle | cancel, hold to run |
| Square / L / R | aim |
| Triangle / Start | inventory |
| Select | options |
| Right stick flick | next / previous weapon (WEAPON SWAP on) |
| L + R + Select | port settings |

Tapping the top right corner of the front screen also opens the port settings. Holding SELECT
while the game boots opens the [mod manager](#mod-manager).

## Settings

<p align="center">
  <img src="screenshots/overlay.png" alt="Settings" width="700"/>
</p>

The settings panel has three pages, flipped with L and R. It opens on QUICK SAVES, and the game
keeps running behind it.

* **QUICK SAVES:** three quick save slots with a thumbnail of where you saved. They're regular
  saves of the room you're in, so they're safe to use anywhere you can move.
* **VIDEO:** aspect (4:3, Integer 640x480 or Stretch), nearest or linear filtering, gamma, MSAA
  (applies on the next boot) and the frame rate.
* **GAMEPLAY:** auto run, weapon swap on the right stick, skip the door animations, the status
  and weapon HUD, HUD auto hide, and EDIT HUD, where you drag the two HUD boxes around with a
  finger and pinch them to size. On a PS TV pick one with Square, move it with the D-Pad and size
  it with L/R. MOD MANAGER restarts the game into the mod manager.

Everything is saved to the `[Vita]` section of `config.ini`, which you can also edit by hand.

### 60 FPS

`FrameRate=60` is the default (FRAME RATE on the VIDEO page switches back to 30). It works the way REuncap does on the PC: the game
still runs at its original 30 ticks a second, so nothing gets faster, but an extra frame is
drawn between ticks with every 3D model halfway between where it was and where it is. Menus,
movies and the backgrounds stay at 30. It turns VSync on while it's set, and adds about 16 ms of
input lag.

### HUD auto hide

With `HudAutoHide=1` the HUD fades out after a few seconds of nothing happening and comes back
when you get hit, shoot, aim or change weapon.

## Mod manager

<p align="center">
  <img src="screenshots/modmanager.png" alt="Mod manager" width="700"/>
</p>

The mod manager runs before the game starts. Open it with the **Mod Manager & Settings** button
on the LiveArea page, by holding **SELECT** while the game boots, or from MOD MANAGER in the
settings panel.

* **MODS:** every folder in `ux0:data/RE1/mods/`. X turns a mod on or off, Square and Triangle
  change the load order (the first one wins when two replace the same file). Each mod says what
  it's doing, and in red when it does nothing for the version you're playing.
* **GAME:** USA or JPN, original or Director's Cut (when `JPN/` and `DC/` are there).
* **VIDEO** and **GAMEPLAY:** the same options as the in-game panel, plus HD TEXTURES and HD
  TEXTURE SIZE.

START saves it all to `config.ini` and starts the game.

## Mods

A mod is a folder in `ux0:data/RE1/mods/` holding a copy of the parts of the game tree it changes,
with the same folder and file names. Rooms (`stage1/room1060.rdt`), backgrounds
(`stage1/rc1060.pak`), textures, sounds, voices and movies all work. A mod can also keep its files
under `USA/` or `JPN/` inside its folder, the way the HQ audio mod ships.

You can turn them on in the mod manager, or by hand:

```ini
[Mods]
Load=SHDP,HQ_Audio,Movies
```

The full list of tested mods and how to install each one is in [docs/MODS.md](docs/MODS.md).

### HD packs

<p align="center">
  <img src="screenshots/hdpack.png" alt="Seamless HD Project" width="700"/>
</p>

The Classic REbirth background packs, the **Seamless HD Project** and **TeamX's HD mod**, work:
copy the pack's `hires/` folder into a mod folder (`mods/SHDP/hires/bgd`, `mods/SHDP/hires/mask`).
Backgrounds and the masks over them are swapped in for 591 room cameras. Pictures are decoded on a
background thread and scaled to `HdMaxHeight` (480, 720 or 960, 720 by default), so a camera cut
can show the original background for a moment on the first visit. The packs' character and item
textures aren't supported yet.

### Movies

Movies can be `.mp4` (H.264 with AAC audio): `mods/Movies/movie/capcom.mp4`. The Vita's decoder
only plays 4:2:0 video, so packs exported as High 4:4:4 (or 10-bit, or HEVC) are skipped and the
original movie plays; the mod manager tells you when that happens. Re-encode them on a PC with
[`tools/vita/convert_movies.sh`](tools/vita/convert_movies.sh):

```bash
tools/vita/convert_movies.sh path/to/pack/movie out/Movies/movie 720
```

It needs `ffmpeg` and keeps the file names, so the output folder goes straight into `mods/`.

## AI Assistance

Parts of the Vita port were written or improved with the assistance of AI (Claude, by Anthropic).
All AI-assisted code was reviewed, tested on hardware, and manually validated, with the overall
implementation and debugging process remaining under human supervision.

## Credits

* **Capcom** for Resident Evil. This project isn't affiliated with Capcom.
* **ecruells** and everyone on the [Resident Evil PC decompilation](https://github.com/ecruells/resident-evil-pc-decomp),
  which this is built on. The PC build's own README is in `docs/PC_README.md`.
* **Rinnegatamante** for vitaGL and vitaShaRK.
* The **VitaSDK** team.
* **NobodyCatchMe-AZ92** for REuncap, which the 60 FPS mode follows.
* The **Seamless HD Project** team (MoArtis, Kayael, Doze_D, FrankWesker) and **TeamX** for the HD
  packs the port can load. Neither is included.

## Building from source

You need [VitaSDK](https://vitasdk.org) with vitaGL, vitaShaRK and the ffmpeg package installed
through vdpm, and `VITASDK` set:

```bash
cmake -S . -B build/vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/vita -j
```

The VPK ends up as `build/vita/residentevil.vpk`. The LiveArea art and template (with the mod
manager button) are in `vita/sce_sys/`, and `vita/hd_map.txt`, the HD pack room map, is packed
into the VPK. `tools/vita/hd_map.py` rebuilds it and `tools/vita/make_menu_font.py` the mod
manager's font.

The Windows and Linux builds still work the same as upstream, see `docs/PC_README.md`.
