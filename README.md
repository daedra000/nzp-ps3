# Nazi Zombies: Portable — PS3

This fork ports NZ:P to the **PlayStation 3** as native homebrew (PSL1GHT), from the
Vril engine in [`vril/`](vril) with a PS3 platform in
[`vril/source/platform/ps3`](vril/source/platform/ps3).

**Status: preview build.** Playable start to finish in Solo: menus, map select, all 19
maps load (Nacht der Untoten played into the late rounds; the others only briefly so
far), sound effects, menu and round music, DualShock 3 controls with PlayStation
button prompts and rumble, the PS3 account name on the main menu ("Welcome, <name>"),
settings that are saved and survive updates, and custom maps downloaded straight
from the main menu. Single player only.
Reports are welcome: send `nzp_log.txt` (see below) with what happened.

**Video**
- 1280x720 at 60 fps in normal play with dips in busy moments.
- Video Options > RESOLUTION: the 3D view at 1280x720 (default), 960x540 or 848x480,
  scaled to the TV. The HUD and menus are always drawn at full resolution.
- Video Options > GAMMA brightens or darkens the game world only; the HUD and menus
  keep their colours.
- Video Options > DYNAMIC RES (on by default): when the graphics chip can't keep up
  (late rounds full of explosions and fire), the 3D view is drawn smaller, down to
  5/8, and scaled back up; the HUD stays sharp. It goes back up when there's room.
- These take effect on APPLY; BACK drops the change.

**System**
- The PS button pauses a match (the pause menu is up when you come back).
- XMB screenshots work: the screen buffers sit on 1 MB boundaries and the RSX never
  waits on a flip the system is holding (both from Grid Siege, where a screenshot
  used to freeze the console).
- Switching PS3 user picks up the new account's name, unless you typed a name of your
  own.

**Custom maps**
- Main menu > DOWNLOAD MAPS lists the community maps from the
  [NZ:P map releases](https://github.com/nzp-team/nzportable/discussions/categories/map-releases)
  (89 so far). Press one to download and install it; it shows up in
  SOLO > CUSTOM MAPS straight away, no restart.
- DOWNLOAD MAPS > MANAGE MAPS lists the installed custom maps; press one to delete it.
- The list is rebuilt daily from those release posts by
  [`tools/maps_list.py`](tools/maps_list.py) (the `Map list` workflow) and published
  to the `maps-list` branch. Maps are downloaded from where their authors posted them.
- Downloads use HTTPS through mbedTLS: the PS3's own HTTPS is too old for GitHub and
  archive.org. If the PS3's date is wrong, certificates are rejected.

**Known issues**
- Late rounds with big crowds and explosions can dip to around 45-50 fps for a
  moment. Explosions, fire and sprites right in front of the camera are now kept
  smaller to help; this is the main thing being worked on.
- Maps other than Nacht der Untoten have had little testing.
- Very big map downloads are unpacked in memory and haven't been tested much yet.
- The crash handler can't install on retail/HEN firmware (liblv2dbg refuses), so a
  crash only ends the log; freezes are caught by the watchdog instead.

## Install
1. A PS3 that runs homebrew (HEN or CFW).
2. Download `nzp-ps3.pkg` from the latest successful
   [PS3 build](../../actions/workflows/ps3.yml) run (artifact `nzp-ps3`).
3. Install it from Package Manager. It shows up as *Nazi Zombies: Portable*.

Everything lives in `/dev_hdd0/game/NZPP00001/USRDIR/`: the game data in `nzp/`
(most of it in `nzp/pak0.pak`), your settings in `nzp/config.cfg` (saved on every
APPLY and when you leave a settings menu; updates don't overwrite it, and deleting it
goes back to the defaults in `config_default.cfg`), and `nzp_log.txt`,
the log to send when something goes wrong (the previous run's is kept as
`nzp_log.old.txt`). The main menu shows the build it came from
(`2.0.1-preview+ps3-<commit>`).

### Installing custom maps by hand
DOWNLOAD MAPS is the easy way. For a map that isn't in its list:
1. Download a map from the [NZ:P map releases](https://github.com/nzp-team/nzportable/discussions/categories/map-releases) page.
2. Extract the `.zip` with WinRAR or 7-Zip.
3. By FTP, copy the map's folders (`gfx`, `maps`, `sounds` and so on) into
   `/dev_hdd0/game/NZPP00001/USRDIR/nzp/`, merging them with the folders already there.
4. Restart the game if it's open. The map should now show up in map select.

Map art goes in `/dev_hdd0/game/NZPP00001/USRDIR/nzp/gfx/menu/custom/`.
Custom maps are made by the community, so some will have bugs.

Note: bigger maps are heavier on the PS3. If the frame rate struggles, drop the
resolution in Video Options (960×540 or 848×480) and lower Max FPS.

### What the log shows
- `[slow]`: anything that held up a frame for more than 60 ms (model, sound, music
  loads, map spawn steps).
- `[alive]`: every 30 s, fps and where each frame's time goes (input, server, world,
  entities, particles, 2D) and the audio thread's load.
- `[video]` / `[gl]`: every 10 s, frame time, time waiting for the RSX, draws and
  vertices per frame.
- `[watchdog]`: if the game stops making progress for 6 s, the last 24 steps it took.
- `missing file`: a file the game looked for and didn't find (the first 50).
- `[fifo]`: on an RSX hang, where the command reader stopped.

## Controls (DualShock 3)
| Button | Action |
|---|---|
| Left stick / right stick | Move / look |
| R2 / L2 | Fire / aim |
| R1 / L1 | Grenade / Bouncing Betty |
| Cross | Jump |
| Square | Reload (hold: use) |
| Triangle | Switch weapon |
| Circle | Crouch / prone (dive while sprinting) |
| R3 / L3 | Knife / sprint |
| Start | Pause |

All rebindable in the game's controls menu.

## How the port works
- **Rendering:** the PS3 has no OpenGL driver. `rsx/` is a GL 1.x layer on the RSX
  taken from [TyrQuakeCell](https://github.com/stingziz0u/tyrquakecell) (via Quake2PS3
  and [IoQuake3-PS3](https://github.com/Mayo1970/IoQuake3-PS3)), with linear fog and
  the texture matrix added for NZ:P. The RSX programs are NV40 assembly, assembled by
  PSL1GHT's `cgcomp -a` at build time (`tools/build_shaders.py`).
- **Fill rate:** the render targets and depth buffer sit in the RSX's tiled memory
  regions (plain rows made every blended or depth-tested pixel slower; this alone
  took 1280x720 from 30 to 60 fps at the worst spots). World, model and sprite
  textures have mipmaps. Gamma is a pass over the 3D view before the HUD is drawn.
- **Big-endian:** NZ:P had only run on little-endian CPUs; the loaders were fixed for
  the Cell PPU (PCX skins, palettes, Half-Life BSP lightmaps, PAK headers).
- **Command buffer:** librsx moves to the next 32 KB piece of the command buffer from
  inside its functions through an asm call GCC 13 doesn't treat as clobbering memory,
  and the commands written right after came out corrupted (RSX hangs). The port makes
  that move itself before every batch (`PS3_Cmd_Reserve` in `ps3_video.c`) so librsx
  never has to, and long index lists go in pieces that fit.
- **CPU:** the Cell PPU stalls on every int/float conversion, so alias models (the
  zombies) go from bytes to floats through a table and straight into the GL layer's
  fast path; each model's triangle list is cached instead of rebuilt per draw.
- **Audio:** one audio port at 48 kHz: Quake's mixer plus the MP3 music (minimp3,
  decoded on the audio thread, which needs a 256 KB stack for it).
- **Map downloader:** `ps3_https.c` is a small HTTP/1.1 client on the PS3's sockets
  with TLS 1.2 from mbedTLS 2.28 (built by CI), checking certificates against
  Mozilla's roots. `ps3_mapdl.c` fetches the list, downloads a zip on a worker thread
  and unpacks it into `nzp/` (map art moved to `gfx/menu/custom/`), recording what it
  installed in `maps/<map>.mapdl` so MANAGE MAPS can remove it.
- **Log:** written by a low-priority thread once the game runs: a direct HDD write
  can take 2 s.
- **Data:** a PKG install drops entries past a limit, so the 1100+ data files are
  packed into `pak0.pak` (`tools/make_pak.py`); what the engine opens by path stays
  loose. PlayStation button prompts are the default (`cl_controllerglyphs "sony"`).

## Building
GitHub Actions ([`.github/workflows/ps3.yml`](.github/workflows/ps3.yml)) builds the
PS3 toolchain once (cached), compiles progs.dat from
[nzp-team/quakec](https://github.com/nzp-team/quakec), takes the PC data from
[nzp-team/assets](https://github.com/nzp-team/assets) (both at pinned revisions),
adds the compiled maps (`.bsp`, which the assets repo doesn't have) from the official
PC nightly build, and makes `nzp-ps3.pkg` on every push. The artifact is the PKG
only.

Crash addresses from a log can be named by running the workflow by hand with them as
its `addrs` input (`ppu-addr2line` against the same commit's ELF).

Locally, with `PS3DEV`/`PSL1GHT` set:
```
cd vril
make -f Makefile.ps3 PS3PY_DIR=$PSL1GHT_SRC/tools/ps3py DATA_DIR=/path/to/nzp pkg
```
`PS3PY_DIR` points at PSL1GHT's `tools/ps3py` with `pkgcrypt` built: the C `pkg`
tool silently drops entries past 1024.

## Licence
This port is free software, released under the GNU General Public License v2
([`LICENSE`](LICENSE)), the same licence as the Quake engine NZ:P is built on.

NZ:P game assets (maps, models, textures, sounds) belong to the NZ:P Team and are
covered by their own terms: https://github.com/nzp-team/assets

Based on NZ:P by the NZ:P Team: https://github.com/nzp-team/nzportable

---

# Nazi Zombies: Portable

# About
This is the main/hub repository for NZ:P, a Call of Duty: Zombies "de-make" powered by various enhanced forks of the Quake engine, in development since 2009. This hub repository serves as a place to host nightly builds as well as a means of bug reporting on a game-wide (non-component) scale. See [the breakdown](#github-organization-breakdown) for source code and other components.

The game itself is feature-equivalent with Call of Duty: World at War on a generic level. Gameplay components are implemented with minor parity differences. Most World at War maps and their unique features are not yet represented. Various small additions and changes from Call of Duty: Black Ops are present as a means of gameplay smoothing, but not on a wide scale. NZ:P is, first and foremost, a Call of Duty: World at War remake.

# Supported Platforms
* Linux (x86, x86_64, armhf, arm64)
* Nintendo Switch
* Nintendo 3DS
* PlayStation Portable (both "PHAT" (PSP-1000) and "SLIM" (PSP-2000 and higher))
* PlayStation VITA
* [WebGL](https://nzp.gay)
* Windows (x86, x86_64)
* TI NSPIRE (CX II)

# GitHub Organization Breakdown
* [assets](https://github.com/nzp-team/assets): Game GFX, Sound, etc. data.
* [vril-engine](https://github.com/nzp-team/vril-engine): The NZ:P engine.
* [quakec](https://github.com/nzp-team/quakec): The game-side code for things like weapons and Perk machines.
* [tools](https://github.com/nzp-team/tools): Misc. development tools.
* [toolbox](https://github.com/nzp-team/toolbox): AiO Development suite for map makers and contributors.

# Screenshots

<center>
    <p float="left">
        <img src="screenshots/0.webp" width="400" />
        <img src="screenshots/1.webp" width="400" /> 
    </p>
    <p float="left">
        <img src="screenshots/2.webp" width="400" />
        <img src="screenshots/3.webp" width="400" /> 
    </p>
</center>

# Credits

#### Programming
Blubswillrule, Jukki, DR_Mabuse1981, Naievil, Cypress, Scatterbox

#### Models
Blubswillrule, Ju\[s]tice, Derped_Crusader

#### GFX
Blubswillrule, Ju\[s]tice, Cypress, Derped_Crusader

#### Sounds/Music
Blubswillrule, Biodude, Cypress, Marty P.

#### Special Thanks
* Spike, Eukara, Contributors: FTEQW
* Shpuld: CleanQC4FTE, heavy dQuakePlus optimization
* Crow_Bar, st1x51: (a)dQuake(plus)
* fgsfdsfgs: Quakespasm-NX
* MasterFeizz: ctrQuake
* Rinnegatamante: Initial VITA port, VITA Auto-Updater
* Ralf Willenbacher: nQuake
* Azenn: GFX Assistance
* BCDeshiG: Extensive Testing

# Stars
<a href="https://www.star-history.com/?repos=nzp-team%2Fnzportable%2Cnzp-team%2Fvril-engine%2Cnzp-team%2Fquakec%2Cnzp-team%2Fassets%2Cnzp-team%2Fnzp-team.github.io&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/image?repos=nzp-team/nzportable%2Cnzp-team/vril-engine%2Cnzp-team/quakec%2Cnzp-team/assets%2Cnzp-team/nzp-team.github.io&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/image?repos=nzp-team/nzportable%2Cnzp-team/vril-engine%2Cnzp-team/quakec%2Cnzp-team/assets%2Cnzp-team/nzp-team.github.io&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/image?repos=nzp-team/nzportable%2Cnzp-team/vril-engine%2Cnzp-team/quakec%2Cnzp-team/assets%2Cnzp-team/nzp-team.github.io&type=date&legend=top-left" />
 </picture>
</a>
