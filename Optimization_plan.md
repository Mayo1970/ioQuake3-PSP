# PSP Optimization Plan (after the Q3DM11 memory work)

Status on 2026-09-27. Read [Optimization_PSP.md](Optimization_PSP.md) for the earlier work.
Each option needs approval before work starts. Section 5 records every result that worked on hardware.

## 1. Current state

Q3DM11 with 4 bots (Lucy, Biker, Patriot, TankJr) boots on a PSP-2000 at `r_picmip 1`. The bots have distinct models.

Memory round (build-verified and hardware-tested, not committed to git):

- `code/renderergl1/tr_bsp.c` streams the BSP lumps one step at a time. It does not load the full 8.3 MB file into hunk temp memory.
- The renderer shares the collision-map visibility data (-1278 KB on Q3DM11).
- `R_LoadPlanes` no longer allocates twice the plane count.
- MD3 models load LOD1 only (`PSP_MD3_LOD` in `code/psp/psp_platform.h`).
- All mono sounds use ADPCM. `com_soundMegs` is 1.
- `PSP_HUNK_RESERVE_MB` is 12, so the hunk is 22 MB (was 19 MB).
- `max_routingcache` is 1024 KB. `code/botlib/be_aas_route.c` enforces it on the PSP.
- DXT1/DXT5 textures (`code/psp/psp_dxt.c`). Set `r_pspDxt 0` to disable.
- JPEG files decode at 1/2^picmip size.
- ADPCM decode cache (`code/psp/psp_adpcm.c`): 32 chunks, 256 KB of heap. Set `s_pspAdpcmCache 0` to disable.
- Bug fix: `FS_FreeFile` freed the streamed BSP block during a load step. `FS_PSP_HoldTempMemory` now prevents this.

Profiling round (build-verified and hardware-tested, not committed to git):

- Counters for file access, load stages and server work (`Sys_PSP_Count*` in `code/sys/sys_psp.c`). Section 4 explains how to read them.
- The renderer profile prints the 10 most expensive shader batches in each 5 s window.
- The renderer profile samples 1 frame in 4 (was 1 in 16).

Load round (build-verified and hardware-tested, not committed to git):

- Step 4.1: file lookups skip the Memory Stick folder check when the path's top folder does not exist in `baseq3`. `CL_InitCGame` went from 43.9 s to 20.6 s. See section 2.5.
- Step 4.2: `FS_FOpenFileByMode` reads use the shared pk3 handle. The bot load went from 7.1 s to 5.0 s. See section 2.6.
- Step 3a: wave deform, `tcMod turb` and `tcMod scroll` do their per-vertex math in float. `slime1` went from 24 µs to 2.9 µs per vertex. See section 2.6.

Load round 2 (build-verified and hardware-tested, not committed to git):

- Step 4.3a: the renderer reads the BSP in one forward pass (was two). The renderer reads 7.7 MB less and the load takes about 1 s less. See section 2.7.
- Step 4.3b, fast mode: the DXT encoder skips the least-squares refine. The DXT encode went from 4.2 s to 2.8 s. Set `r_pspDxtFast 0` to use the refine again. See section 2.7.
- Step 3c: `deformVertexes bulge` and `deformVertexes normal` do their per-vertex math in float. Q3DM11 does not use these deforms, so the hardware test is still open.
- Step 4.3d, first part: counters for the bot character, chat and weight file loads. The weight files take 4.0 s of the 5.0 s bot load. See section 2.7.

Frame-rate round (build-verified and hardware-tested, not committed to git):

- B1: `bot_thinktime` is 150 on the PSP (was 100). `PSP_BOT_THINKTIME` in `code/psp/psp_platform.h` sets it in `sv_bot.c` and `ai_main.c`. The cvar stays cheat-protected, so `autoexec.cfg` cannot change it. See section 2.8.
- B4: one server frame builds at most 6 area routing caches (`PSP_AAS_FRAME_ROUTING_BUILDS`, `code/botlib/be_aas_route.c`). A route that needs a missing cache waits a frame. Cached routes are not blocked. See section 2.8.

## 2. Measured results

Source: `q3psp22.log` from build `q3dm11-profile-debug`. The CPU ran at 383 MHz through a plugin. The log still prints 333 MHz, because it reads the value from the power service.

The counters do not change the result: `CL_InitCGame` took 43.91 s, against 43.89 s without them.

### 2.1 Memory after load

| Item | Value |
|---|---|
| Heap arena | 33775 of 35072 KB (about 1.3 MB spare) |
| Hunk free | 4009 KB |
| Zone free | 1991 KB (lowest 984 KB, largest block 1098 KB) |
| Sound free | 1561 KB |
| Textures | 2493 KB (1785 KB DXT) |
| World data | 3585 KB (not reliable, see section 2.7) |
| Lowest hunk free during world load | 5587 KB |

### 2.2 Load time

The full map load takes about 61 s: about 6 s in `SV_SpawnServer`, 8.3 s for the bots, and 46.6 s for the renderer restart and `CL_InitCGame`.

`CL_InitCGame` (43.9 s):

| Cost | Count | Time | Share |
|---|---|---|---|
| Memory Stick folder check before each lookup | 1111 | 23.2 s | 53% |
| pk3 read and decompress (39.6 MB) | 2953 | 7.5 s | 17% |
| DXT encode (mip levels) | 1442 | 4.2 s | 10% |
| pk3 entry open | 705 | 0.9 s | 2% |
| New pk3 handle (`unzOpen`) | about 15 | about 0.5 s | 1% |
| Rest: decode, parse, resample | | about 7.6 s | 17% |

- `sv_pure 0` is set at boot, and the loose `baseq3` folder is first in the search order. So every lookup first calls `stat` and `fopen` on `ms0:/PSP/GAME/QUAKE3/baseq3/<path>`. Each check costs about 21 ms. `baseq3` has no asset folders, so all these checks fail.
- 386 lookups find no file at all: 287 `.tga`, 52 `.md3`, 35 `.skin`, 6 `.iqm`, 6 `.mdr`. The folder check is almost all of their cost.
- The counters overlap: image load 18.9 s (417 images, with their lookups), image create 4.8 s (with DXT), world 18.0 s (with the world textures).

Other load phases:

| Phase | Folder checks | New pk3 handles |
|---|---|---|
| Boot to menu | 66, 1.4 s | about 0.2 s |
| `SV_InitGameProgs` | 18, 0.4 s | 18, about 0.66 s |
| Bot load (8.3 s) | 60, 1.3 s | 60, about 2.0 s |
| Renderer and UI restart | 55, 1.1 s | none |

- A new pk3 handle costs about 34 ms. `FS_FOpenFileByMode` reads (botlib, `trap_FS_FOpenFile`) always open one.
- The map-select menu spends 0.8-0.9 s per 5 s window on folder checks (16 missing levelshot `.tga` files).
- The rest of the 8.3 s bot load (about 5 s) is not measured. It is probably the parsing of the bot character and chat files.

### 2.3 Fights

Values in ms per frame. The `slime1` column is per sampled frame. These values are from before step 3a. Section 6.2 has the current fight costs.

| Window | Channels | Frame | gfxCPU | slime1 | svBots | svTrace | Sound paint |
|---|---|---|---|---|---|---|---|
| Calm | 0-4 | 22-30 | 7-12 | 0 | 3.2-6.0 | 1.2-3.1 | 1-2 |
| Fight at the slime | 18 | 61.8 | 26 | 14.1 | 10.1 | 4.4 | 10.7 |
| Fight at the slime | 26 | 85.4 | 33 | 16.8 | 14.2 | 5.9 | 19.1 |
| Fight away from the slime | 14-16 | 36-37 | 13 | 0 | 5.2-5.3 | 2.0-2.2 | 6-7 |

- **Renderer:** `textures/liquids/slime1` costs 14-17 ms per frame when it is in view. That is about half of gfxCPU. The shader (`scripts/liquid.shader` in `pak0.pk3`) has `tessSize 32`, `deformVertexes wave`, `cull disable` and 3 passes with `tcMod turb`. The deform growth in the earlier log was this shader. Away from the slime, a fight adds only about 3-4 ms of rendering (player models).
- **Bot AI:** 130-200 ms per second of play, in calm and in fights. It looks larger per frame in fights only because the frame rate drops.
- **Routing cache:** 206-247 KB of the 1024 KB limit, and no route is ever thrown away. Rebuilds happen only when the match starts (one 213 ms hitch). The limit is not a problem.
- **Traces:** 700-1000 per second at about 75 µs each (50-100 ms per second).
- **Sound paint** is still the second-largest fight cost: 19 ms at 26 channels, with 75-89% ADPCM cache hits in fights.

### 2.4 Sound paint (memory round)

| Channels | Before cache | With cache |
|---|---|---|
| 27-28 | 42.0 ms | 22.2 ms |
| 17-19 | 26.3 ms | 13.5 ms |
| 9-14 | 12.3 ms | 8.7-9.7 ms |

### 2.5 Load time after step 4.1

Source: `q3psp22.log` from build `q3dm11-loose-skip-debug`. `baseq3` has no subfolders (`PSP loose folders … : 0`), so every folder check under a subfolder is skipped. `q3config.cfg` still loads.

| Phase | Before | After | Change |
|---|---|---|---|
| `CL_InitCGame` | 43.9 s | 20.6 s | -23.3 s |
| Renderer restart and `CL_InitCGame` window | 46.6 s | 22.1 s | -24.5 s |
| Bot load | 8.3 s | 7.1 s | -1.2 s |
| `SV_SpawnServer` (longest frame) | 6.0 s | 5.5 s | -0.5 s |
| Full map load | about 61 s | about 35 s | about -26 s |
| Map-select menu, file lookups per 5 s | 0.84 s | 0.02 s | |
| One sound file open | about 22 ms | 0.2-3 ms | |

What is left in `CL_InitCGame` (20.6 s):

| Cost | Time | Share |
|---|---|---|
| pk3 read and decompress (39.6 MB) | 7.4 s | 36% |
| DXT encode | 4.2 s | 20% |
| pk3 entry open | 0.9 s | 5% |
| New pk3 handles (`unzOpen`) | about 0.5 s | 2% |
| Rest: decode, parse, resample | about 7.6 s | 37% |

- The bot load still spends about 2.0 s on new pk3 handles (60 opens). The rest of the bot load, about 5 s, is not measured.
- Fights did not change, as expected. One fight at the slime: 21 channels, 65 ms per frame, `slime1` 7.8 ms, ADPCM hits 66%.

### 2.6 Results after steps 4.2 and 3a

Source: `q3psp22.log` from build `q3dm11-handle-slime-debug`.

Load time:

| Phase | After 4.1 | After 4.2 | Change |
|---|---|---|---|
| `SV_SpawnServer` (longest frame) | 5.5 s | 4.8 s | -0.7 s |
| `SV_InitGameProgs` file lookups | 680 ms | 51 ms | -0.6 s |
| Bot load | 7.1 s | 5.0 s | -2.0 s |
| `CL_InitCGame` | 20.6 s | 20.2 s | -0.4 s |
| Full map load | about 35 s | about 31 s | about -3.3 s |

- New pk3 handles in `CL_InitCGame`: 2 (the BSP stream and one other), 116 ms. The bot load has none.
- Physical pk3 handle restores in `CL_InitCGame` went from 110 to 22.
- The rest of the bot load (about 4.8 s) has almost no file cost now. It is probably the parsing of the bot files.

`slime1` cost:

| Build | Vertices per frame | Cost per sampled frame | Cost per vertex |
|---|---|---|---|
| Before 3a (3 windows) | 325-702 | 7.8-16.8 ms | 24 µs |
| After 3a (3 windows) | 236-593 | 0.7-1.7 ms | 2.9 µs |

- The `deform` counter in fights went from 1.5-5.7 ms to 0.2-0.4 ms per sampled frame.
- The menu banners (`q3banner02`, wave deform) went from 0.45 µs to 0.21 µs per vertex.
- Worst 5 s window in this run: 12 channels, 47.3 ms per frame (21 FPS). The earlier runs had 65-85 ms. The fights are not the same, so this is not an exact comparison.
- Near the slime, the renderer cost is now spread over many shaders. No single shader costs more than about 1.7 ms per frame.
- A routing burst still causes one hitch of about 220-230 ms: at the match start, and once later when a bot entered a new area (`aasAreaCache` 132 rebuilds in one window).

### 2.7 Results after load round 2

Source: `q3psp22.log` from build `q3dm11-onepass-dxtfast-debug`. `autoexec.cfg` was not present, so `r_pspDxtFast` was 1.

`CL_InitCGame` (after 4.2 and 3a, then after load round 2):

| Counter | Before | After | Change |
|---|---|---|---|
| `CL_InitCGame` | 20.16 s | 17.80 s | -2.36 s |
| `world` | 10141 ms | 8231 ms | -1.91 s |
| `fsRead` time | 7309 ms | 6329 ms | -0.98 s |
| `fsRead` bytes | 39649844 | 31604943 | -8044901 |
| `fsRead` calls | 2953 | 2829 | -124 |
| `dxt` (2664960 texels) | 4207 ms | 2831 ms | -1.38 s (-33%) |
| `imageCreate` | 4815 ms | 3450 ms | -1.37 s |
| Lowest hunk free during the world load | 5587 KB | 4414 KB | -1173 KB |

- 4.3a: the `fsRead` bytes went down by exactly 8044901. That is the end of the entity lump, so the second pass is gone. The lowest hunk free matches the estimate (about 4413 KB).
- 4.3b: 1.58 µs to 1.06 µs per texel. The first analysis expected about half, the result is one third. The look of the textures is not checked yet.
- The `world` counter contains both gains: about 0.98 s from 4.3a and about 0.93 s from the DXT encode of the world textures.
- No permanent memory cost: after `CL_InitCGame`, the hunk (4009 KB free), the zone (1989 KB free) and the heap (33714 KB used) are the same as before.
- Other phases do not change: `SV_SpawnServer` 4.85 s, bot load 5.04 s. The full map load goes from about 31 s to about 29 s.
- The `PSP world: N KB of world data` line went from 3585 KB to 5070 KB, but the hunk use after the load is the same. The line measures the distance between two `Hunk_Alloc( 0, h_low )` pointers. `Hunk_SwapBanks` can move permanent allocations to the other hunk side while temp memory is in use, so the line changes with the temp-memory pattern. Do not use this line as a memory figure.

Bot load (the 5041 ms frame with the `loaded ... from bots/...` lines):

| Counter | Calls | Time | Slowest call |
|---|---|---|---|
| `botWeight` (4 item and 4 weapon weight files) | 8 | 4029 ms | 979 ms |
| `botChat` | 4 | 724 ms | 190 ms |
| `botChar` | 4 | 129 ms | 53 ms |
| Sum | | 4882 ms (97% of the frame) | |

- The weight files are 83% of the bot load. The item weight files (`bots/*_i.c`) include `fw_items.c`. It uses the `*_SCALE` macros 218 times, and each use expands to three `$evalfloat` directives, so each item file evaluates 654 expressions. `fw_weap.c` (weapon weights) has none.
- `PC_Directive_evalfloat` (`code/botlib/l_precomp.c`) writes each result with `sprintf( "%1.2f" )`. On the PSP, `%f` formats through soft-float double code. This is the probable cause, but it is not measured.
- Fights did not change. Worst 5 s window: 7 channels, 41.4 ms per frame (24 FPS). `slime1` stays at about 3.0 µs per vertex.

### 2.8 Results after B1 and B4

Source: `q3psp22.log` from build `q3dm11-think150-routebudget-debug`. Twelve 5 s windows after the match start. The fights are not the same as in section 6.2, so the frame values are not an exact comparison.

Bots (`svBots`), without the first window:

| Value | Before (section 6.2) | After |
|---|---|---|
| Bot time per second | 135-175 ms | 92-129 ms |
| Per fight frame (7-17 channels) | 5.0-7.2 ms | 2.4-5.2 ms |
| Per calm frame (2-4 channels) | 2.9-4.2 ms | 2.4-3.6 ms |
| Traces per second | 700-940 | 596-885 |

- B1: the bot time per second went down by about 30%. The bots think 33% less often, so most of `svBots` is per-think work.

Routing in the first window after the match start (B4):

| Counter | Value |
|---|---|
| `aasAreaCache` | 135 builds, 184 ms, slowest 9.5 ms, mean 1.4 ms |
| `aasPortalCache` | 1184 calls, 195 ms (most calls stopped at the budget) |
| `aasBudget` | 1241 refused route queries |
| Most bot work in one frame (`svBots` max) | 32.0 ms |

- There are no refused queries after the first window. Later windows build 1-6 area caches, at most 0.6 ms each.
- The slowest build is 9.5 ms, so 6 builds in one frame can cost about 20 ms. The estimate of about 10 ms was too low.
- The worst frame of the first window is still 213 ms, but it is not server work: `svFrame` peaks at 36 ms. This log does not show where the frame goes. So the 213 ms match-start hitch in sections 2.3 and 2.6 may not be the routing.
- No routing burst happened later in the match (for example, a new travel-flag set), so that case is not tested yet.
- The log cannot show whether the bots stood still during the 1241 refusals.

Frame cost after B1 and B4:

| Windows | Channels | Frame | `svBots` | `soundPaint` |
|---|---|---|---|---|
| Fights | 7-9 | 22.7-30.6 ms | 2.4-3.7 ms | 2.2-3.4 ms |
| Fights | 14-17 | 32.2-41.1 ms | 3.4-5.2 ms | 5.2-8.2 ms |
| Calm | 2-3 | 22.0-31.7 ms | 2.4-3.6 ms | 1.3-1.5 ms |

- At 14-17 channels, the sound paint now costs more than the bots.
- No audio underruns during play. The count stays at 1579 from the end of the load to the quit.

## 3. Options

Suggested order of the open items: 4.3d step 2, the look check of 4.3b, the 3c test on Q3DM4, the rest of 4.3, 3b, Option 2, Option 1.

The frame-rate items are in section 6, with their own order in section 6.7. Option 3b is item B2 there, and Option 1 is item S3.

### Option 1: Increase the ADPCM cache to 64 entries (open, do last)

- **Priority:** last. The heap has little spare memory, so the other options come first (Matt, 2026-09-27).
- **Problem:** in fights the hit rate falls to 70-89%. A paint can use two chunks per channel, so 28 channels can need up to 56 chunks. The cache holds 32.
- **Change:** set `PSP_ADPCM_CACHE_ENTRIES` to 64 in [code/psp/psp_adpcm.c](code/psp/psp_adpcm.c) (line 8).
- **Cost:** 256 KB more heap. The 1.3 MB spare was measured with the 256 KB cache already in place, so about 1.0 MB stays spare. 48 entries cost 128 KB more.
- **Risks:** the cache allocates first, so a later heap allocation fails instead (a texture spill, or a larger map). The gain is not proven: chunk misses on new sounds stay at any cache size.
- **Note:** each paint repaints about 9 times the audio it plays (section 6.3). Item S1 in section 6.4 makes each paint shorter, so it also lowers the chunk pressure. Test S1 first.
- **Test:** Q3DM11 with bots. Compare the `PSP paint:` lines in fight windows against section 2.3.

### Option 2: `four.dm_68` A/B of the cache (open)

- **Purpose:** a clean measurement of the cache. The demo is deterministic, so both runs play the same frames.
- **Limits:** the demo does not use Q3DM11, runs no bots, and does not test the memory goal.
- **Steps:**
  1. Put `set s_pspAdpcmCache 1` in `baseq3/autoexec.cfg`.
  2. Boot. Start `four` from the Demos menu. Let it play to the fraglimit.
  3. Copy `q3psp22.log` off the Memory Stick. Rename it to `demo-cache1.log`.
  4. Put `set s_pspAdpcmCache 0` in `baseq3/autoexec.cfg`.
  5. Do steps 2 and 3 again. Rename the log to `demo-cache0.log`.
- **Compare:** discard the first 30 s. Align six 5-second windows by obituary text. Keep the WLAN state the same in both runs.

### Option 3: Fight cost (profiled, see section 2.3)

- **3a. Make `slime1` cheaper (done, hardware-tested 2026-09-27, see section 2.6):**
  - Cause: the per-vertex math of `deformVertexes wave`, `tcMod turb` and `tcMod scroll` used soft-float doubles. `tess.shaderTime` is a `double`, and `WAVEVALUE` casts to `int64_t`. `slime1` runs this math 7 times per vertex (1 deform, 3 turb, 3 scroll) on about 700 vertices per frame.
  - Change: [code/renderergl1/tr_shade_calc.c](code/renderergl1/tr_shade_calc.c). `RB_PSP_WaveFraction` reduces the time phase in double once per call. The vertex loops then use only float and `int`. The disassembly shows no soft-double call inside the three loops.
  - Look: no change expected. At most, a table index can move by one entry out of 1024. Every shader with a wave deform, turb or scroll gets faster, for example water, lava and banners.
  - Not changed: `deformVertexes bulge` and `deformVertexes normal` still use doubles per vertex. `slime1` uses neither.
  - Result: `slime1` 24 µs to 2.9 µs per vertex, so about 14-17 ms to 0.7-1.7 ms per frame.
- **3c. The other per-vertex doubles (done, build-verified 2026-09-27, hardware test open):**
  - Change: [code/renderergl1/tr_shade_calc.c](code/renderergl1/tr_shade_calc.c) and [code/renderercommon/tr_noise.c](code/renderercommon/tr_noise.c).
    - Bulge: `RB_PSP_Fraction` reduces the time to a table position in double once per call. The vertex loop uses only float and `int`.
    - Normal: the noise time is split once per call. `R_PSP_NoiseGet4fSplit` takes the split time, so the loop calls no double math. The result is bit-identical to the old code.
  - The disassembly shows no soft-double call inside the two vertex loops.
  - Look: normal deform, no change. Bulge, at most one table index moves by one entry out of 1024.
  - The gain depends on which shaders in a map use these deforms. The `deform` line in `PSP rprof:` shows it.
  - Test: Q3DM11 uses neither deform. In `pak0.pk3`, Q3DM4 has both (`textures/gothic_block/gkcspinemove` bulge, `models/mapobjects/flag/banner_strgg` normal). Q3DM6, Q3DM8 and Q3Tourney2 have the banner, and the CTF maps have the flags (normal).
- **3b. Bot AI (open):** the only simple setting is `bot_thinktime`, and bots then react more slowly. Anything more needs scopes inside `BotAIStartFrame`: `BotAI` per bot, the botlib frame, and the bot user commands.

### Option 4: Load time (profiled, see section 2.2)

- **4.1 Skip the folder check when it cannot find anything (done, hardware-tested 2026-09-27, see section 2.5):**
  - Change: [code/qcommon/files.c](code/qcommon/files.c) records the top-level folders of each loose search directory when it adds that directory (`FS_AddGameDirectory`, from the existing `.pk3dir` listing). `FS_PSP_LooseMayExist` skips `Sys_FOpen` for a lookup under a folder that is not in the list. Paths without a folder (`autoexec.cfg`, `q3config.cfg`) are always checked. `FS_CreatePath` adds a folder to the list when the game creates one (screenshots, demos). A pure server skips the listing, so every check stays there.
  - Result: `CL_InitCGame` 43.9 s to 20.6 s, full map load about 61 s to about 35 s. All 1111 checks in `CL_InitCGame` were skipped, and `q3config.cfg` still loads.
  - Risk: low. A loose file still works when its folder exists in `baseq3`. A folder that a USB copy adds needs a restart of the game.
  - Build: `build-out-q3dm11/loose-skip/EBOOT.PBP` (`q3dm11-loose-skip-debug`). Debug and RelWithDebInfo compile with no new warnings.
- **4.2 Reuse the shared pk3 handle (done, hardware-tested 2026-09-27, see section 2.6):**
  - Cause: `FS_FOpenFileByMode` (botlib and `trap_FS_FOpenFile`) always asked for a new pk3 handle (`unzOpen`, about 34 ms).
  - Change: [code/qcommon/files.c](code/qcommon/files.c). On the PSP, `FS_FOpenFileByMode` reads use the shared pk3 handle. `FS_PSP_SharedPakHandleBusy` already opens a new handle when another open file holds the shared one, so two open files cannot move each other's read position. A new counter, `fsUnzOpen`, counts the new handles that are still opened.
  - Not changed: the BSP stream (`tr_bsp.c`) and sound streams keep their own handle, because they stay open while other files load.
  - Result: full map load about 35 s to about 31 s. Bot load 7.1 s to 5.0 s.
- **4.3 Later (open):** pk3 read and decompress (7.3 s for 39.6 MB, 6.3 s for 30.1 MB after 4.3a), DXT encode (4.2 s, 2.8 s after 4.3b), and the bot file parsing (about 4.9 s). Parts, with costs (2026-09-27 analysis, gains not measured):
  - **4.3a BSP in one pass (done, hardware-tested 2026-09-27, see section 2.7):**
    - Cause: the renderer decompressed the 8.1 MB BSP twice (about 15.8 MB). After the surface lumps, it went back for the leaf, node, leaf-surface, model, visibility-header, light-grid and entity lumps. q3map writes these lumps before, between and after the surface lumps.
    - Change: [code/renderergl1/tr_bsp.c](code/renderergl1/tr_bsp.c). After the planes, one forward read fills two hunk temp blocks. The surface lumps go to the upper block, which is freed after `R_LoadSurfaces`. The other lumps go to the lower block, which is kept until the light grid loads. Of the visibility lump, the pass reads only the 8-byte header. The loaders and their order do not change.
    - Why more than the 660 KB of the first analysis: the model, entity and light-grid lumps also lie between the surface lumps. Without them, the second pass stays.
    - Cost: about 1174 KB more hunk temp at the peak on Q3DM11. The lowest hunk free during the world load goes from 5587 KB to about 4.4 MB. The hunk free after the load does not change.
    - Gain: about 1 s. `CM_LoadMap` decompresses the same file in 1.02 s.
    - With `r_vertexLight 0`, the lightmap read still causes one seek back, so the file is read twice (was three times).
    - Risk: when the CM does not hold the map's clusters, the visibility lump is read with one seek back, as before.
    - Result: the renderer reads 8044901 bytes less (the whole second pass). `fsRead` 7309 ms to 6329 ms. Lowest hunk free during the world load 4414 KB.
  - **4.3b DXT encode:** 1.6 µs per texel, and the encoder has no double math.
    - **Fast mode (done, hardware-tested 2026-09-27, look check open, see section 2.7):** [code/psp/psp_dxt.c](code/psp/psp_dxt.c) skips the least-squares refine when `r_pspDxtFast` is 1 (the default). Each block then skips the refine and the second index pass. Set `r_pspDxtFast 0` in `baseq3/autoexec.cfg` to use the refine again for a look comparison.
    - Result: `dxt` 4207 ms to 2831 ms (1.58 µs to 1.06 µs per texel).
    - Look check (open): compare the textures with `set r_pspDxtFast 0` in `baseq3/autoexec.cfg` and without it.
    - A DXT cache on the Memory Stick (open) costs about 2 MB per map, a slower first load and a check for changed pk3 files, and could save 6-8 s on later loads.
  - **4.3c Sounds:** 8.1 MB of 22 kHz WAV files are read, resampled and ADPCM-encoded (about 3 s). Pre-converted 11 kHz ADPCM files in a PSP pk3 need a converter tool and a new loader, and could save about 2.5 s.
  - **4.3d Bot file parsing:** about 4.9 s.
    - Counters (done, hardware-tested 2026-09-27, see section 2.7): [code/server/sv_game.c](code/server/sv_game.c) times the four bot load syscalls. `botChar` is `BotLoadCharacter`, `botChat` is `BotLoadChatFile`, `botWeight` is `BotLoadItemWeights` plus `BotLoadWeaponWeights`.
    - Result: weights 4029 ms, chat 724 ms, character 129 ms. Together 97% of the 5041 ms bot load frame.
    - Step 2 (open, needs approval): time `PC_Directive_evalfloat` and its `sprintf( "%1.2f" )` to confirm the cause. If it is the `sprintf`, format the value with integer math (the value times 100, rounded, as `%d.%02d`). The weight values then must stay the same, so compare the tokens against the old output.

## 4. Build and test notes

- Only Debug builds write `q3psp<PSP_LOG_GEN>.log` (`con_psp.c` is `#ifndef NDEBUG`). RelWithDebInfo writes no log, and the counters compile only in Debug builds.
- `PSP_LOG_GEN` is 22 in [cmake/platforms/psp.cmake](cmake/platforms/psp.cmake). Each boot overwrites the log, so copy it after every run. The first log line gives the build ID.
- The log goes to a 256 KB RAM buffer and is written when the game quits. Keep a test run to about 2 minutes, and quit from the menu.
- Build from PowerShell (repository mounted at `/src`):

  ```
  docker run --rm -v "E:\Users\Matteo\Desktop\quake3\PSP\ioQuake3-PSP:/src" pspdev/pspdev:latest sh -c "psp-cmake -S /src -B /tmp/b -DCMAKE_BUILD_TYPE=Debug -DPSP_PERF_BUILD_ID=<id> && cmake --build /tmp/b -j8 && mkdir -p /src/build-out-q3dm11/<dir> && cp /tmp/b/Debug/EBOOT.PBP /tmp/b/Debug/ioquake3.elf /src/build-out-q3dm11/<dir>/"
  ```

- After each build, check two things:
  1. The build output has no `stubs out of order` warning.
  2. `psp-objdump -s -j .rodata.sceResident ioquake3.elf | grep -c ForKernel` prints 0.
- Builds:
  - `build-out-q3dm11/think-route/EBOOT.PBP`: the current test build. The onepass-dxtfast build plus B1 and B4 (`q3dm11-think150-routebudget-debug`).
  - `build-out-q3dm11/onepass-dxtfast/EBOOT.PBP`: the handle-slime build plus 4.3a, 4.3b fast mode, 3c and the 4.3d counters (`q3dm11-onepass-dxtfast-debug`).
  - `build-out-q3dm11/handle-slime/EBOOT.PBP`: step 4.1 plus 4.2 and 3a (`q3dm11-handle-slime-debug`).
  - `build-out-q3dm11/loose-skip/EBOOT.PBP`: the profile build plus step 4.1 (`q3dm11-loose-skip-debug`).
  - `build-out-q3dm11/profile/EBOOT.PBP`: the profile build (`q3dm11-profile-debug`), without step 4.1.
  - `build-out-q3dm11/debug/EBOOT.PBP`: the memory-round build, without the counters.
  - Do not use `build-out-q3dm11/EBOOT.PBP`. It is the old picmip 3 build with the BSP corruption bug.
  - You can delete `build-out-q3dm11/bisect/`. It is no longer needed.
- Do not use Git Bash `sed -i` on source files. It changes CRLF line endings to LF.

How to read the counters:

- `PSP count [<where>]:` lines give calls, total ms and the slowest call since the previous report. `<where>` is a load phase (the heap report points and `before CL_InitCGame`) or `window` (each 5 s frame report, with µs per frame).
- The counters overlap. For example, `fsLookup` contains `fsLoose` and `fsPackOpen`, and `world` contains the world texture loads.
- `fsLooseSkip` counts the folder checks that step 4.1 skipped. It has no time, because a skipped check costs nothing.
- `fsUnzOpen` counts the new pk3 handles (`unzOpen`). Builds before `q3dm11-handle-slime-debug` do not have it.
- `botChar`, `botChat` and `botWeight` time the bot file loads (step 4.3d). Cached loads count as calls with almost no time. Builds before `q3dm11-onepass-dxtfast-debug` do not have them.
- `aasBudget` counts the route queries that the B4 budget refused. Builds before `q3dm11-think150-routebudget-debug` do not have it.
- `PSP miss:` lines list the first 48 files that were not found anywhere since the previous report.
- `PSP rprof:   shader` lines give the cost of a shader from `RB_BeginSurface` to `RB_EndSurface`, per sampled frame. `(table full)` collects the shaders after the first 48.

## 5. Results log

| Date | Change | Result on hardware | Source |
|---|---|---|---|
| 2026-09-27 | ADPCM decode cache, 32 entries | Sound paint at 27-28 channels: 42.0 ms to 22.2 ms | `q3psp22.log`, memory round |
| 2026-09-27 | Memory round (section 1) | Q3DM11 with 4 bots boots at `r_picmip 1` | `q3psp22.log`, memory round |
| 2026-09-27 | Profile counters | No measurable cost: `CL_InitCGame` 43.91 s against 43.89 s | `q3psp22.log`, `q3dm11-profile-debug` |
| 2026-09-27 | Step 4.1: skip the Memory Stick folder check | `CL_InitCGame` 43.9 s to 20.6 s; full map load about 61 s to about 35 s; bot load 8.3 s to 7.1 s | `q3psp22.log`, `q3dm11-loose-skip-debug` |
| 2026-09-27 | Step 4.2: reuse the shared pk3 handle | Bot load 7.1 s to 5.0 s; full map load about 35 s to about 31 s | `q3psp22.log`, `q3dm11-handle-slime-debug` |
| 2026-09-27 | Step 3a: float per-vertex wave, turb and scroll | `slime1` 24 µs to 2.9 µs per vertex (14-17 ms to 0.7-1.7 ms per frame) | `q3psp22.log`, `q3dm11-handle-slime-debug` |
| 2026-09-27 | Step 4.3a: BSP in one pass | The renderer reads 8044901 bytes less; `fsRead` 7309 ms to 6329 ms; lowest hunk free during the world load 5587 KB to 4414 KB | `q3psp22.log`, `q3dm11-onepass-dxtfast-debug` |
| 2026-09-27 | Step 4.3b: DXT fast mode | `dxt` 4207 ms to 2831 ms (1.58 µs to 1.06 µs per texel); with 4.3a, `CL_InitCGame` 20.16 s to 17.80 s | `q3psp22.log`, `q3dm11-onepass-dxtfast-debug` |
| 2026-09-27 | Step 4.3d: bot load counters | Bot load 5041 ms: weights 4029 ms, chat 724 ms, character 129 ms | `q3psp22.log`, `q3dm11-onepass-dxtfast-debug` |
| 2026-09-27 | B1: `bot_thinktime` 150 | Bot time 135-175 ms to 92-129 ms per second; per fight frame 5.0-7.2 ms to 2.4-5.2 ms | `q3psp22.log`, `q3dm11-think150-routebudget-debug` |
| 2026-09-27 | B4: 6 area-cache builds per server frame | Match-start routing burst (135 builds, 184 ms) spread over several frames; most bot work in one frame 32 ms. The 213 ms worst frame stays, outside the server | `q3psp22.log`, `q3dm11-think150-routebudget-debug` |

## 6. Frame rate

Added 2026-09-27 as analysis. B1 and B4 are done and hardware-tested (section 2.8). Each other item needs approval.

### 6.1 Goal

- Raise the frame rate in fights on Q3DM11 with 4 bots.
- The earlier project target was 30 FPS, which is 33.3 ms per frame ([Legacy/TODO.md](../Legacy/TODO.md)).
- Now: fights run at 24-30 FPS (33-41 ms per frame). Calm play runs at 41-46 FPS (22-25 ms per frame).

### 6.2 Fight frame cost

Source: `q3psp22.log` from build `q3dm11-onepass-dxtfast-debug`. Five fight windows (7-16 sound channels) and two calm windows (1-4 channels).

| Cost (ms per frame) | Fights | Calm | Notes |
|---|---|---|---|
| Frame | 33.1-41.4 | 21.7-24.6 | |
| Renderer back end (`gfxCPU`) | 11-16 | 8 | `backendSurfs` has 6-9 ms with no timer in fights (see M1) |
| cgame (`cgameVM`) | 6-7 | 4-5 | Contains the renderer front end (`cgameScene` 1.9-4.3, of which `world` 1.2-3.5) |
| Bots (`svBots`) | 5.0-7.2 | 2.9-4.2 | 135-175 ms per second |
| Game frame (`svGame`) | 1.5-2.0 | 1.1-1.2 | 20 Hz, contains the bot `Pmove` |
| Snapshots (`svSnap`) | 1.6-1.9 | 1.5-1.6 | 5 snapshots at 20 Hz (4 bots, 1 player) |
| Traces (`svTrace`) | 2.0-3.2 | 1.2-2.0 | Inside `svBots` and `svGame`, so do not add it. 66-87 µs per trace, 700-940 per second |
| Sound paint (`soundPaint`) | 3.7-6.2 | 1.0-1.6 | Inside `other` |

- In fights, the renderer is 32-40% of the frame, the server 25-27%, and the sound paint 10-17%.
- The soft-float double scan of the ELF shows no double math inside these hot loops. A few per-batch calls stay (`RB_BeginSurface`, `RB_RenderDrawSurfList`).

### 6.3 Finding: the mixer paints about 9 times the audio it plays

- `SNDDMA_Init` sets `dma.submission_chunk` to 1 ([psp_snd.c:379](code/psp/psp_snd.c#L379)). So `S_GetSoundtime` moves `s_paintedtime` back to "now + `s_mixPreStep`" (50 ms) on every frame ([snd_dma.c:1364](code/client/snd_dma.c#L1364)).
- `S_Update_` then paints up to about 10 frame times ahead ([snd_dma.c:1402](code/client/snd_dma.c#L1402)). Only `s_mixahead` limits it, and the PSP default is 0.5 s ([snd_dma.c:1701](code/client/snd_dma.c#L1701)).
- So each frame paints again most of what the frame before it painted. The work per frame grows with the frame time, up to the 0.5 s limit.
- The log agrees in every window: samples per call are about 110 × frame ms − 551. A 41 ms frame paints 3966 samples, but it needs only 452 new samples at 11025 Hz.
- The paint costs 0.10-0.14 µs per sample and channel (about 40-50 cycles). The loop itself needs about 15. The 32 KB paint buffer does not fit the 16 KB data cache, and `S_PaintChannels` clears all 32 KB on each call ([snd_mix.c:677](code/client/snd_mix.c#L677)).
- [Legacy/TODO.md:848](../Legacy/TODO.md#L848) says `s_mixahead` does not change the samples per second. That is wrong for this backend.

### 6.4 Options

**Sound**

| # | Change | Estimated gain in fights | Cost and risk |
|---|---|---|---|
| S1 | `set s_mixahead 0.2` in `baseq3/autoexec.cfg`. No code. | Paint −1.6 to −3.2 ms per frame (samples per call go from 3110-3966 to 1654). In the older 26-channel fight of section 2.3 (85 ms frames, 19.1 ms paint): about −12.7 ms. | A frame longer than about 170 ms gets a short silence, for example the 220-230 ms routing hitch (section 2.6). With 0.25: margin about 220 ms, gain about 40%. With 0.15: margin about 120 ms, gain about 70%. Compare the `PSP paint:` lines and `PSP audio: N underruns`. |
| S2 | Mix in blocks of 512-1024 samples (`PAINTBUFFER_SIZE` in [snd_local.h:29](code/client/snd_local.h#L29), PSP only) and clear only the painted span. | Estimate: 20-40% of the paint that stays after S1. Not measured. | Small change in `snd_mix.c`. No audible change. |
| S3 | Option 1: 64 ADPCM cache entries. | Small. Hits are already 90-97% in this log, and S1 lowers the chunk pressure. | 256 KB of heap. Keep it last. |

**Bots and server**

| # | Change | Estimated gain in fights | Cost and risk |
|---|---|---|---|
| B1 | **Done: 150, section 2.8.** `bot_thinktime` 150 or 200 (cvar). | 150: −1.7 to −2.4 ms. 200: −2.5 to −3.6 ms. This assumes that the per-think work is most of `svBots`. B2 checks it. | Bots aim and steer less often, so they play worse. Matt decides. |
| B2 | Option 3b: timers inside `BotAIStartFrame`: `BotAI` per bot, the botlib update, `BotFindEnemy`/`BotEntityVisible`, `BotAimAtEnemy`, `trap_BotMoveToGoal`, the goal choice, `BotChooseWeapon`, `BotCheckSnapshot`. | None by itself. It selects the bot fixes. Candidates from the code: `BotEntityVisible` does up to 3 traces per hidden enemy, and upstream commented out its PVS test ([ai_dmq3.c:2851](code/game/ai_dmq3.c#L2851)). Skill-4 aim runs `AAS_PredictClientMovement` ([ai_dmq3.c:3423](code/game/ai_dmq3.c#L3423)). | One debug build. |
| B3 | Build the bot snapshots at 10 Hz (bot `snapshotMsec` 100), not 20 Hz. | −0.4 to −0.8 ms (estimate). Bots build 4 of the 5 snapshots, but read only the newest one ([sv_bot.c:629](code/server/sv_bot.c#L629)), once per think ([ai_dmq3.c:5032](code/game/ai_dmq3.c#L5032)). | Low. Entity events stay valid for 300 ms. |
| B4 | **Done: 6 builds per server frame, section 2.8.** A PSP limit on the routing updates per frame. The upstream limit is commented out ([be_aas_route.c:1671](code/botlib/be_aas_route.c#L1671)). | Removes the 213-230 ms hitch. The mean does not change. | Bots can choose a worse goal for some frames. |
| T1 | Trace counters by caller (bot AI, game frame, player move) and by part (world, entity clip). | None by itself. It points B2 and any trace work. | Counters only. |

**Renderer**

| # | Change | Estimated gain in fights | Cost and risk |
|---|---|---|---|
| R1 | Guard-band clip planes: clip against k × the screen size (start with k = 4), not against the screen edges ([psp_draw.c:819](code/psp/psp_draw.c#L819)). The GE draws in a 4096 × 4096 space around the screen ([psp_glimp.c:359](code/psp/psp_glimp.c#L359)), and the scissor removes what is off the screen. | `drawClip` is 1.6-2.0 ms. Estimate: −0.8 to −1.6 ms. Calm play gets faster too. | Never tried. Check all four screen edges on hardware. A vertex behind the eye still fails a guard plane, so the damage at the bottom of the screen with `r_pspClip 0` (Legacy/Q3PORT.md, line 3397) does not come back. Keep k below 8.5 (x) and 15 (y). |
| R2 | Reject a triangle when all three corners are outside the same plane (AND of the outcodes), before the clip ([psp_draw.c:1426](code/psp/psp_draw.c#L1426), [psp_draw.c:1473](code/psp/psp_draw.c#L1473)). | 0.5 ms or less. | The picture does not change. |
| R3 | Effect cvars: `cg_noProjectileTrail 1` (`smokePuff` costs 0.4-0.8 ms in rocket fights), `cg_draw3dIcons 0` (removes 2 of the 3 `RE_RenderScene` calls per frame), `cg_simpleItems 1`. The first-person gun costs 0.6-1.6 ms (`rocketl` the most). | As listed. | The game looks different. Matt decides. |
| M1 | Timers that split `backendSurfs` into the surface fill (the `rb_surfaceTable` calls into `tess`) and the flush (`RB_EndSurface`). | None by itself. 6-9 ms of the back end has no timer now. | About 140 timer calls per sampled frame. |

### 6.5 Media Engine for audio

- Sessions A1-A6 (August 2026, [Legacy/VME.MD](../Legacy/VME.MD)) built an ME mixer. A6 gave output identical to the CPU mixer on a PSP-2000. A6 was synchronous, so it could not save time. A7 (asynchronous) was closed without a test. The A6 code and logs are not in this tree.
- For: the ME is idle, and the main CPU is the limit. With the paint buffer in the ME's own memory, the bus load is small (about 3 MB/s). The ME can paint 0.5 s ahead at no main-CPU cost, so the silence risk of S1 goes away.
- Against: A6 did PCM only. All mono sounds are ADPCM now, so the decoder and its cache must move to the ME too. The work also needs `kcall.prx`, a channel snapshot per frame, cache maintenance for reused sound buffers, sleep and wake handling, and the CPU fallback. A2-A6 took about five sessions.
- Gain: 3.7-6.2 ms per fight frame now, and about 1.5-3 ms after S1. `soundRest` (0.8-1.3 ms) stays on the CPU.
- Decision: do S1 first. Reopen the ME only if the paint stays above about 2 ms in fights. A reopen overrides the NO-GO in `Legacy/TODO.md` and `Legacy/VME.MD`.

### 6.6 VFPU investigation

Sources: [Legacy/VFPU.MD](../Legacy/VFPU.MD) (August 2026), `vfpu-docs/docs/reference-outputs/inst_cycles.txt` (PSP-3000) and `mem_cycles.txt` (PSP-1000), the disassembly of `build-out-q3dm11/onepass-dxtfast/ioquake3.elf`, and `q3psp22.log`.

**VFPU code in use now**

| Kernel | State |
|---|---|
| `sceGum` matrices (`pspgum_vfpu`) | On |
| `PSP_VertexOutcode` (4 `vdot.q`) | On |
| `PSP_ClipIntersectVFPU` | On. `drawClip` −13.6% (Session 13a) |
| `VectorArrayNormalize` (`PSP_NORMALIZE_VFPU`) | Validated on hardware, but OFF in `cmake/platforms/psp.cmake` |
| `RB_CalcDiffuseColor_vfpu` (`PSP_DIFFUSE_VFPU`) | Rejected: it corrupted the shading. OFF |
| MD3 XYZ lerp | Rejected twice: the logo and the models were not drawn. Removed |

**Hardware facts**

- `vadd`, `vmul`, `vscl`: 1 cycle per instruction, 5 cycles latency. `vdot`: 1.2 and 7. Scalar `vsin`, `vcos`, `vrsq`, `vsqrt`: 1 and 7.
- `vsin` and `vcos` have an absolute error of 4.8e-7. The `vasin` error rises to 2e-2 near ±1, so the VFPU has no usable `atan2`.
- Memory: a random miss costs about 72 cycles. Sequential reads out of the cache: `lw` 0.88 bytes per cycle, `lv.q` 2.0 bytes per cycle. The data cache is 16 KB.
- So the VFPU speeds up float math and long sequential reads. It does not speed up pointer chasing, branches or integer work. The ME has no VFPU.

**Fight costs against VFPU fit**

| Cost in fights | Kind of work | VFPU fit |
|---|---|---|
| Bots 5.0-7.2 ms | Branches, AAS tree walks, many small calls | No, except the trigonometry (V5) |
| Traces 2.0-3.2 ms | BSP tree walk, branches per brush side | No |
| Snapshots 1.6-1.9 ms | Integer work and copies | No |
| Sound paint 3.7-6.2 ms | Integer mix, the 32 KB buffer streams out of the cache | Weak. S1 and S2 remove the waste in plain C |
| Back end, timed kernels | Outcode, clip, pack, colors, MD3, diffuse | Partly (V1-V3) |
| Back end, 6-9 ms with no timer | Surface fill (333-724 faces per frame), dispatch, state | Unknown until M1 |
| Front end `world` 1.2-3.5 ms | Node walk, up to 4 plane-box tests per node | Maybe (V6). Count the nodes first |

**Candidates**

| # | Change | Now (fights) | Estimated gain | Risk and notes |
|---|---|---|---|---|
| V1 | Outcode with `vcmp.q` + `mfvc`, and 2-4 vertices per asm block. The compiled loop now does one vertex at a time: `lvl.q`/`lvr.q`, `vone.s`, 4 `vdot.q`, 4 `mfv` with `nop`s, 11 integer instructions, `sb`. | `drawOutcode` 0.56-0.95 ms | 0.2-0.5 ms | Low. Same bits. Put one VFPU instruction between `vcmp` and `mfvc`. This is item 4 of Legacy/TODO.md. |
| V2 | MD3 lerp: XYZ blend, normal blend and normalize in one VFPU loop. Keep the `sinTable` lookups scalar. | `md3Lerp` 0.69-1.27 ms | 0.2-0.5 ms | Medium-high. Two earlier tries failed, and the cause is not recorded. It needs a startup self-test against the scalar loop. |
| V3 | Diffuse color: one asm loop, without the VFPU → GPR → VFPU round trip per vertex. | `diffuse` 0.24-0.67 ms | 0.1-0.3 ms | Medium. The earlier try corrupted the shading. |
| V4 | Surface fill (`RB_SurfaceFace`) with `ulv.q`/`sv.q`. `points` sits at offset 40 in `srfSurfaceFace_t`, so the source is not 16-byte aligned (`ulv.q` is safe on the Slim). | No timer | Unknown | Do M1 first. |
| V5 | `vsin`/`vcos` in `AngleVectors` (3 `sinf` + 3 `cosf` per call), `RotatePointAroundVector` and `BG_EvaluateTrajectory`. newlib `sinf`/`cosf` take about 100-150 cycles each with the range reduction (estimate from the code size). | Call counts not measured | 0.1-0.4 ms (estimate) | Low-medium. `Pmove` uses `AngleVectors`, so the results differ slightly from a PC server. Count the calls first. `atan2f` (`vectoangles`) stays scalar. |
| V6 | Front-end node cull: test the 4 frustum planes against the node box with one `vtfm4.q` and one `vcmp.q`. | `world` 1.2-3.5 ms | 0.3-1.0 ms, low confidence | The node walk is pointer chasing. Only the plane math gets faster. Count the nodes and the plane tests first. |
| V7 | Turn on `PSP_NORMALIZE_VFPU` (already validated). | `normalize` 0.01-0.12 ms | Up to 0.1 ms | None. |

**Verdict**

- If V1, V2, V3 and V5 all work, the VFPU saves about 1-2 ms per fight frame. V4 and V6 can add more only if M1 and the node count show float-bound work.
- That is less than S1, B1 or R1. The fight frame is limited by memory latency, branches and repeated work, not by float math.
- Put V1 and V7 in the low-risk build. Keep V2, V3, V5 and V6 until the measurements are in.

### 6.7 Suggested order

1. S1: add `set s_mixahead 0.2` to `baseq3/autoexec.cfg`. No build. Compare the `PSP paint:` lines and the underrun count against section 6.2.
2. Measurement build: B2, T1, M1, and call counters for V5 and V6.
3. Low-risk build, each item with its own counter: S2, R2, B3, V1, V7. For a change under 2 ms, use paired `four.dm_68` runs (lesson in Legacy/TODO.md).
4. R1 in its own build, with a check of the four screen edges.
5. Matt decides R3. B1 is done (150, section 2.8).
6. Then, from the measurements: the bot fixes, V2/V3/V5/V6, and the ME mixer (only if the paint stays above 2 ms).

If S1, B1 (150), B3 and R1 all work, a fight frame gets about 4.5-7 ms shorter: 33-41 ms goes to about 27-36 ms. The gains are not proven to add up.
