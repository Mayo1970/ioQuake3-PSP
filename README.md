# ioquake3-PSP

A port of [ioQuake3](https://github.com/ioquake/ioq3) to the Sony PSP, targeting
**PSP-2000 (Slim) and later** (PSP-3000, PSP Go) with a custom GL-to-GU translation
layer (OpenGL 1.1 fixed-function → `sceGu`/GE)

**PSP-1000 (Fat) is not supported** as the port needs 64 MB. Fat models only have 32.

### **This port doesn't work on PPSSPP**

## Status

- World geometry, textures, sky and particles render correctly at 480×272
- Sound effects, music and cinematic audio supported.
- Networking: LAN discovery, internet server browser, hosting, and play.
- Analog nub for aim, face buttons + shoulder buttons for movement (see
  **Controls** below)
- **No on-screen keyboard / text entry.**
- Framerate hovers around 30 FPS. Don't expect to play in big maps with many bots however.
- Forced V-blank for online play due to limitations. 

## Why a new port instead of [Crow_bar's PSPQuake3](https://github.com/Crow-bar/PSPQuake3)

[Crow-bar's PSPQuake3](https://github.com/Crow-bar/PSPQuake3) is the first
working PSP Quake 3 port, but being ~16 years old meant it missed many features that have been explored more deeply. This port takes advantage of **everything** the PSP can give to it to squeeze every inch of memory off the OS. Besides that, it has unlocked networking features (Crow-Bar's port did as well, but it was only through direct connection, so no server browser), better optimization as well as being easier to compile and work on a modern PSP SDK.

## Building

Just use Docker lol

```
docker pull pspdev/pspdev:latest

docker run --rm -v "${PWD}:/src" -w /src pspdev/pspdev:latest sh -c `
  "psp-cmake -S /src -B /tmp/b -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
   cmake --build /tmp/b -j8 && \
   cp /tmp/b/RelWithDebInfo/EBOOT.PBP /tmp/b/RelWithDebInfo/ioquake3.elf /src/build-out/"
```

## Installing on PSP


Requires CFW with large-memory mode enabled (this port builds against
`_PSP_FW_VERSION=600`); tested on ARK.


Buy the base game legally [Here](https://www.gog.com/en/game/quake_iii_arena), then copy the `.pk3` files from your
install/disc into the paths below

File structure:

```
ms0:/PSP/GAME/ioquake3/EBOOT.PBP
ms0:/PSP/GAME/ioquake3/baseq3/pak0.pk3 ... pak8.pk3
```



## Controls

The port uses a similar, if not, almost identical, button layout as close as possible as the Dreamcast port.

### In-game

| Input | Action |
|---|---|
| Nub | Aim (yaw/pitch) |
| **Triangle** | Forward |
| **Cross** | Backward |
| **Square** | Strafe left |
| **Circle** | Strafe right |
| **D-pad Left** | Previous weapon |
| **D-pad Right** | Next weapon |
| **D-pad Down** | Crouch |
| **D-pad Up** | Reserved, unbound |
| **L** | Jump |
| **R** | Attack |
| **Start** | Menu / pause |
| **Select** | Scoreboard |
| **Select + Triangle** | Toggle the Quake console |

### Menus / console

| Input | Action |
|---|---|
| Nub / D-pad | Move cursor / arrow keys |
| **Cross** | Confirm (Enter) |
| **Circle** | Back (Escape) |
| **Start** | Escape |
| **L / R** | Scroll list up/down |
---

---

## Memory budget

Requires CFW large-memory mode: **64 MB** main RAM (PSP-2000 Slim and later
only), of which roughly **39 MB** is claimed as the process heap.

| Region | Size | Notes |
|---|---|---|
| `com_hunkMegs` | 24 MB | Maps, shaders, models |
| `com_zoneMegs` | 5 MB | Dynamic allocs, zlib inflate |
| `com_soundMegs` | 2 units (~6 MB) | One unit lives in volatile RAM; sound evicts least-recently-used on pressure rather than failing |
| Everything else in the heap | remainder | Textures, newlib/stdio, small zone |
| VRAM (GE eDRAM) | **2 MB** | `sceGeEdramSetSize(4 MB)` is attempted but returns an unresolved-import error on this hardware/firmware combination; the texture/VRAM allocator sizes itself from `sceGeEdramGetSize()` at runtime, so it degrades to 2 MB cleanly rather than assuming 4 |
| Framebuffer stride | 512 | Always — the 480×272 screen does not use 480 as the buffer width |

`com_zoneMegs`/`com_soundMegs` are compile-time knobs (`PSP_ZONE_MEGS`,
`PSP_SOUND_MEGS` in `code/sys/sys_psp.c`); `com_hunkMegs` is derived as
`heap (39 MB) - PSP_HUNK_RESERVE_MB (15)`, not set directly. All three are
retuned against measured heap reports rather than guessed.

---

## Overclocking (ARK-5)

Everything above assumes the stock `scePowerSetClockFrequency(333, 333, 166)`
this port boots at. ARK-5's CPU overclock plugin helps this port greatly. Mileage may vary.

### **demo/four.dm_68 benchmark**

| Clock | Min FPS | Max FPS | Avg FPS |
|---|---:|---:|---:|
| 333 MHz (stock) | 18.5 | 48.9 | 33.2 |
| 383 MHz | 22.6 | 55.3 | 39.9 |
| 403 MHz | 25.0 | 56.4 | 42.1 |
| 423 MHz | 27.3 | 57.2 | 44.6 |
| 443 MHz | 30.8 | 57.5 | 47.1 |

---

## Credits

- **[ioQuake3](https://github.com/ioquake/ioq3)** — the upstream engine this port is based on.
- **[PSPSDK](https://github.com/pspdev/pspsdk)** / **[pspdev](https://github.com/pspdev/pspdev)** — the PSP homebrew toolchain and Docker build image.
- **[Crow_bar's PSPQuake3](https://github.com/Crow-bar/PSPQuake3)** — the first working PSP Quake 3 port; source for the proven control scheme, the pk3 handle-limit workaround, and other hardware-tested reference material used throughout this port's development.
- **[DaedalusX64](https://github.com/DaedalusX64/daedalus)** — the N64 emulator; reference for modern PSP techniques (VFPU, Media Engine, VRAM/volatile-memory management) consulted throughout this port's development.

---

## AI disclosure

Parts of this port were developed with the assistance of **Claude** (Anthropic). AI was used for code generation, debugging, porting guidance, and documentation. All AI-generated code was reviewed and tested on hardware before inclusion.

---

## License

ioQuake3 is GPLv2. This port layer is also GPLv2. See `COPYING.txt`.
