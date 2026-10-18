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

## Where to get them

These are the exact downloads the tested list below was made with. Get the PC files; the port only
takes the parts it can use.

| Mod | Download | File to get |
|---|---|---|
| Seamless HD Project 1.1 | [ModDB: Resident Evil - Seamless HD Project](https://www.moddb.com/mods/resident-evil-seamless-hd-project/downloads) | [Resident Evil - Seamless HD Project for PC Mediakite](https://www.moddb.com/mods/resident-evil-seamless-hd-project/downloads/resident-evil-seamless-hd-project-for-pc-mediakite), then [RE SHDP - Patch 1.1](https://www.moddb.com/mods/resident-evil-seamless-hd-project/downloads/re-shdp-patch-1-1) extracted over it |
| Resident Evil HD mod (TeamX) | [ModDB: Resident Evil HD mod](https://www.moddb.com/mods/resident-evil-hd-mod/downloads) | [Resident Evil HD mod](https://www.moddb.com/mods/resident-evil-hd-mod/downloads/resident-evil-hd-mod) (not the mask generator tool) |
| RE1 PC HQ Audio Mod (lexas87) | [Internet Archive: re-1-hq-audio-mod](https://archive.org/download/re-1-hq-audio-mod) | `RE1 PC - HQ Audio Mod (ORIGINAL).zip` for the original music, or `(DUAL SHOCK).zip` for the Dual Shock soundtrack |
| REuncap (not needed) | [GitHub: NobodyCatchMe-AZ92/REuncap](https://github.com/NobodyCatchMe-AZ92/REuncap) | Nothing: the port has the same 60 FPS mode built in |
| Classic REbirth (not needed) | [classicrebirth.com](https://classicrebirth.com/index.php/downloads/resident-evil-classic-rebirth/) | Nothing: it patches the PC program, which the port replaces |

The Seamless HD Project says it is for the Japanese MediaKite PC release. On the Vita that doesn't
matter: its backgrounds and masks are matched to rooms by picture, so they work with the USA (GOG)
files and the Japanese ones alike.

## Tested mods

| Mod | Works | How to install |
|---|---|---|
| **Seamless HD Project 1.1** (MoArtis, Kayael, Doze_D, FrankWesker) | Backgrounds and masks: yes (972 of its 1103 images match a room camera; tested in Vita3K). Movies: 1280x946, untested at that size on hardware; run them through `convert_movies` to get 720-line copies. The room files in `Mod_SHDP/STAGE*` are plain `.rdt` replacements and should load, untested. Character, item and UI textures (`skin`, `skin2`, `misc`) and the `TEXT/*.xml` patches: no. | Copy `hires/` (and `bio1dc/` for DC mode) to `mods/SHDP/`. Optionally `Mod_SHDP/STAGE2` and `STAGE7` to `mods/SHDP/stage2` and `stage7`, and the `convert_movies` output of the pack (see [Movie packs](#movie-packs)). |
| **Resident Evil HD mod** (TeamX, 2022-08-31) | Backgrounds and masks: yes (same names as SHDP, 972 matched). `jpn/movie/*.avi` (H.264 inside AVI): needs `convert_movies`. `skin`, `skin2`, `misc`: no. | Copy `hires/` to `mods/TeamX_HD/`. The `.asi`/`.dll` files are for the PC and aren't needed. |
| **RE1 PC HQ Audio Mod** | Yes, all 602 music, sound and voice files (tested in Vita3K). | Copy the folder as it is (it ships as `JPN/Sound`, `JPN/Voice`) to `mods/HQ_Audio/`, or put `sound/` and `voice/` straight in it. |
| **PS1 movie packs** (`.mp4` sets of the FMVs) | Yes if they are 4:2:0. Packs exported as High 4:4:4 are listed as INACTIVE in the manager until converted. | `mods/<name>/movie/*.mp4`, through `convert_movies` first. |
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
* **VIDEO:** aspect, filter, gamma, antialiasing, frame rate, HD textures on/off, HD texture size
  and HD preload.
* **GAMEPLAY:** auto run, weapon swap, skip doors, status HUD, weapon HUD, HUD auto hide.

START saves everything to `config.ini` and starts the game. Changing antialiasing restarts the app,
since it can only be set at boot.

## HD packs

The PC packs name each picture after a hash their loader computes from a DirectDraw surface, which
the Vita doesn't have. `vita/hd_map.txt` (shipped inside the VPK) maps each room camera to that
name instead: 591 backgrounds and 403 mask sheets. It was built by `tools/vita/hd_map.py`, by
matching pictures against the GOG release and reading the pack's `hires/cache/ROOM*.dat` order.

* Pictures are decoded on a background thread when you enter a room. Without HD preload, a camera
  cut can show the original background for the frames the HD one is still decoding.
* **HD PRELOAD** / `[Vita] HdPreload=1` stops that. When you press START in the mod manager (and
  on any later start while something is missing), every picture of the active packs is decoded once
  into `ux0:data/RE1/hdcache/<size>/`, already scaled, with a progress bar. CIRCLE stops it and
  starts the game; the rest is done next time. In game a picture is then a quick read instead of a
  WebP decode, and the game waits for it instead of showing the original first. For Seamless HD at
  720 lines it takes about 800 MB and a few minutes. Changing HD TEXTURE SIZE builds a new cache
  and deletes the old one; deleting `hdcache/` by hand is always safe.
* `HD TEXTURE SIZE` / `[Vita] HdMaxHeight` (480, 720 or 960) is the height pictures are scaled
  down to when decoded. 720 is the default; 960 is sharper and slower to load.
* Pictures larger than 4096 px on a side are skipped, so an 8K pack won't run the Vita out of
  memory.
* `HD TEXTURES` / `[Vita] HdTextures=0` turns the packs off without removing them.

Model, item and menu textures (`skin`, `skin2`, `misc`) aren't mapped yet.

## Movie packs

The Vita plays `.mp4` movies with its hardware decoder, which only takes H.264 4:2:0 (Baseline,
Main or High), and `.avi` movies with a small decoder that has Cinepak and MPEG-4 but no H.264.
Packs exported as High 4:4:4, 10-bit or HEVC, or H.264 inside AVI, are skipped (the manager says
how many) and the original movie plays instead. Movies taller than 720 lines are played, but
1280x960 decoding plus the colour conversion is a lot for the Vita and isn't tested on hardware,
so the converter scales them down too. The Vita can't re-encode
video itself, so this is done once on a PC.

`tools/vita/convert_movies.py` does it automatically. Point it at a downloaded pack, a mod folder,
or a whole `mods` folder:

```bash
python3 tools/vita/convert_movies.py "RE_SHDP_1.1"
```

On Windows, drag the folder onto `tools/vita/convert_movies.bat`. It finds every `movie` folder,
checks each file with `ffprobe`, copies the ones the Vita already plays, re-encodes only the rest
(H.264 High 4:2:0 + AAC, 720 lines at most, same names as `.mp4`), and writes a ready mod tree next
to the input as `<folder>_vita`. Copy what is inside it to `ux0:data/RE1/mods/`. The game looks for
an `.mp4` of each movie first, so a converted file wins over an unplayable `.avi` of the same name.

Options: `-o OUT` for another output folder, `--max-height 480` for smaller files, `--crf 18` for
higher quality, `--dry-run` to only list what it would do. Stopped runs pick up where they left
off. It needs `ffmpeg` and `ffprobe` with libx264 on the PATH ([ffmpeg.org](https://ffmpeg.org/download.html)).
