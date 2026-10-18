# Mods on the PS Vita port

Every mod lives in its own folder under `ux0:data/RE1/mods/` and is switched on in the mod manager
(or by hand in `config.ini`, `[Mods] Load=first,second`). The first mod in the list wins when two
of them replace the same file.

A mod folder holds one or more of these:

| What | Where in the mod folder | Notes |
|---|---|---|
| Replaced game files | same paths as the game tree: `stage1/room1060.rdt`, `stage1/rc1060.pak`, `sound/BGM_19.wav`, `voice/...`, `item/...` | Any file the game loads by name. Case doesn't matter. |
| Per-region files | `USA/...` or `JPN/...` inside the mod | The running region's folder is searched first, then the mod's root. Sound and voice are also taken from the other region's folder, since they are the same files. |
| Movies | `movie/<name>.mp4` (or `.avi`) | H.264 Baseline/Main/High 4:2:0 with AAC. Anything else is skipped and the original `.avi` plays. See [Movie packs](#movie-packs). |
| HD backgrounds and masks | `hires/bgd/*.webp`, `hires/mask/*.webp` | Classic REbirth texture packs. A Director's Cut session looks in `bio1dc/hires/` first. See [HD packs](#hd-packs). |

## Tested mods

| Mod | Works | How to install |
|---|---|---|
| **Seamless HD Project 1.1** (MoArtis, Kayael, Doze_D, FrankWesker) | Backgrounds and masks: yes (972 of its 1103 images match a room camera; tested in Vita3K). Movies: 1280x946 High 4:2:0, so the port accepts them, but that size isn't tested on hardware; re-encode at 720 if they don't play. The room files in `Mod_SHDP/STAGE*` are plain `.rdt` replacements and should load, untested. Character, item and UI textures (`skin`, `skin2`, `misc`) and the `TEXT/*.xml` patches: no. | Copy `hires/` (and `bio1dc/` for DC mode) to `mods/SHDP/`. Optionally `Mod_SHDP/STAGE2` and `STAGE7` to `mods/SHDP/stage2` and `stage7`, and `movie/` to `mods/SHDP_Movies/movie/` (or its `convert_movies.sh` output). |
| **Resident Evil HD mod** (TeamX, 2022-08-31) | Backgrounds and masks: yes (same names as SHDP, 972 matched). `jpn/movie/*.avi` (H.264 inside AVI): needs `convert_movies.sh`. `skin`, `skin2`, `misc`: no. | Copy `hires/` to `mods/TeamX_HD/`. The `.asi`/`.dll` files are for the PC and aren't needed. |
| **RE1 PC HQ Audio Mod** | Yes, all 602 music, sound and voice files (tested in Vita3K). | Copy the folder as it is (it ships as `JPN/Sound`, `JPN/Voice`) to `mods/HQ_Audio/`, or put `sound/` and `voice/` straight in it. |
| **PS1 movie packs** (`.mp4` sets of the FMVs) | Yes if they are 4:2:0. Packs exported as High 4:4:4 are listed as INACTIVE in the manager until re-encoded. | `mods/<name>/movie/*.mp4`, after `convert_movies.sh` if needed. |
| **REuncap** | Not needed: the port has its own 60 FPS mode built on the same idea (`[Vita] FrameRate=60`, FRAME RATE in the settings). | Nothing to install. |
| **Classic REbirth** | Not needed and not loadable: it patches the PC executable, and this port already is a rebuilt executable. | Nothing to install. |

Mods made of plain file replacements (rooms, backgrounds as `.pak`, `.tim` textures, sounds,
voices) work whichever mod they come from. What can't load is anything that patches the PC
program itself (`.dll`, `.asi`, Classic REbirth XML patches).

## Mod manager

<p align="center">
  <img src="../screenshots/modmanager.png" alt="Mod manager" width="700"/>
</p>

It opens before the game starts, three ways:

* the **Mod Manager & Settings** button on the game's LiveArea page,
* holding **SELECT** while the game boots,
* **MOD MANAGER** on the GAMEPLAY page of the in-game settings (press X twice; the game restarts
  into the manager, so save first).

Pages, flipped with L and R:

* **MODS:** every folder in `mods/`, what it carries and whether it's doing anything. X turns a
  mod on or off, Square and Triangle move it up and down the load order. A mod that does nothing
  for the selected game version (a JPN-only folder while playing USA, a movie pack the Vita can't
  decode) says so in red.
* **GAME:** game version (USA, or JPN when `JPN/` is there) and content (original or Director's
  Cut when `DC/` is there).
* **VIDEO:** aspect, filter, gamma, antialiasing, frame rate, HD textures on/off and HD texture size.
* **GAMEPLAY:** auto run, weapon swap, skip doors, status HUD, weapon HUD, HUD auto hide.

START saves everything to `config.ini` and starts the game. Changing antialiasing restarts the app,
since it can only be set at boot.

## HD packs

The PC packs name each picture after a hash their loader computes from a DirectDraw surface, which
the Vita doesn't have. `vita/hd_map.txt` (shipped inside the VPK) maps each room camera to that
name instead: 591 backgrounds and 403 mask sheets. It was built by `tools/vita/hd_map.py`, by
matching pictures against the GOG release and reading the pack's `hires/cache/ROOM*.dat` order.

* Pictures are decoded on a background thread when you enter a room. A camera cut shows the
  original background for the frames the HD one is still decoding, then switches.
* `HD TEXTURE SIZE` / `[Vita] HdMaxHeight` (480, 720 or 960) is the height pictures are scaled
  down to when decoded. 720 is the default; 960 is sharper and slower to load.
* Pictures larger than 4096 px on a side are skipped, so an 8K pack won't run the Vita out of
  memory.
* `HD TEXTURES` / `[Vita] HdTextures=0` turns the packs off without removing them.

A WebP read from the memory card is slow; expect the first visit to a room to show the original
background for a moment. Model, item and menu textures (`skin`, `skin2`, `misc`) aren't mapped yet.

## Movie packs

The Vita plays `.mp4` movies with its hardware decoder, which only takes H.264 4:2:0 (Baseline,
Main or High). Packs exported as High 4:4:4, 10-bit, or HEVC are skipped (the manager says how many
need re-encoding) and the original movie plays instead.

`tools/vita/convert_movies.sh` re-encodes a folder of movies into ones the Vita can play, with the
same names, so the output drops straight into a mod:

```bash
tools/vita/convert_movies.sh "RE_SHDP_1.1/movie" out/SHDP_Movies/movie 720
```

The third argument is the maximum height (default 960). It needs `ffmpeg` with libx264. It handles
`.mp4`, `.avi`, `.mkv` and `.mov`, so TeamX's H.264-in-AVI Japanese movies go through it too.
Then copy `out/SHDP_Movies` to `ux0:data/RE1/mods/`.
