/*
===========================================================================
PSP port - code/psp/psp_platform.h

Force-included by every translation unit via cmake/platforms/psp.cmake
(-include). Holds the memory-budget floors (Wii wii_platform.h pattern,
Q3PORT.md 1.4/2): #ifndef guards in code/qcommon/common.c pick these up
ahead of upstream's 128/48/128 defaults.

Session 2 values are the mirror's proven Slim numbers (Q3PORT.md 1.2).
The actual com_hunkMegs cvar is computed at runtime from measured free
memory in code/sys/sys_psp.c and injected as a boot command line; these
#defines are only the floor MIN_COMHUNKMEGS protects against a bad
measurement, and the DEF_* fallback if nothing is injected.

The .bss shrinking audit (Wii wii_platform.h pattern) is Session 3 work -
not started here.
===========================================================================
*/

#ifndef __PSP_PLATFORM_H__
#define __PSP_PLATFORM_H__

#define DEF_COMHUNKMEGS   27
#define MIN_COMHUNKMEGS   15
#define DEF_COMZONEMEGS   5

/*
.bss shrinking (Wii wii_platform.h pattern). The full audit is Session 3;
this one entry is pulled forward because it alone blocks Session 2's gate.

s_rawsamples (code/client/snd_dma.c:88) is
[MAX_RAW_STREAMS][MAX_RAW_SAMPLES] portable_samplepair_t == 8 bytes each.
Upstream MAX_RAW_STREAMS is MAX_CLIENTS*2+1 == 129, i.e. 16.9 MB of .bss.
Streams 1..MAX_CLIENTS*2 exist only for VoIP (code/client/cl_parse.c:684,690,
inside #ifdef USE_VOIP), and cmake/platforms/psp.cmake forces USE_VOIP OFF.
Stream 0 carries cinematic audio (cl_cin.c:1154,1164) and background music
(snd_dma.c:1478). 2 streams == 256 KB, saving 16.7 MB.
*/
#define MAX_RAW_STREAMS   2

/*
buckets (code/server/sv_main.c:367) is leakyBucket_t[MAX_BUCKETS], 40 bytes
each. Upstream's 16384 is 640 KB of .bss, sized - per its own comment - "to
make it more of an effort to DoS" an internet-facing dedicated server. This
port is a client with a local listen server and BUILD_SERVER is OFF. Session 9
gave it real sockets, so the rate limiter is no longer dead code - but a PSP
listen server holds a handful of players over 802.11b, not the internet-facing
dedicated server upstream sized this for. 256 entries is 10 KB, saving 630 KB.

MAX_HASHES is a separate constant and stays at 1024: SVC_HashForAddress does
hash &= (MAX_HASHES - 1), which is independent of the bucket count.
*/
#define MAX_BUCKETS       256

/*
===========================================================================
Session 8 .bss cuts.

Enabling sound costs ~3.0 MB per unit: SND_setup (client/snd_mem.c:82-85)
allocates com_soundMegs * 1536 sndBuffers of 2060 bytes. The current budget
uses two units (~6.18 MB total), with one unit reserved in volatile memory
before textures and one unit left in the normal heap. The Session 8 hardware
run spent the first unit from the heap and the heap ran dry - libjpeg
"Insufficient memory (case 4)", PSP_TexUpload2D failing on 4 KB, and
FS_ReadFile handing back zeroed buffers that surfaced as "Not a JPEG file:
starts with 0x00 0x00" and 'Couldn't find "fmt" chunk'.

The heap grows by the same 3 MB (PSP_HEAP_KB, cmake/platforms/psp.cmake) and
these five caps pay for it out of .bss, measured with
psp-nm --size-sort -S. Every one of them is dead weight on this port
specifically, not a quality trade.
===========================================================================
*/

/*
cls.globalServers[MAX_GLOBAL_SERVERS] and the parallel
cls.globalServerAddresses (client/client.h:327,330) are ~1.0 MB of the 1.1 MB
clientStatic_t at upstream's 4096, i.e. ~284 bytes per row.

Session 8 cut this to 32 because psp_net.c was loopback-only and the browser
could not return a single row. Session 9 raised it to 1024 once networking
worked - but that number was never actually load-bearing on a hardware
measurement, and it silently drifted out of sync with q3_ui/ui_servers2.c's
own MAX_GLOBALSERVERS (UI-side storage per master tab), which stayed at 128.
That mismatch is a real bug: a master list past 128 entries gets silently
clobbered into the UI's last slot instead of appearing.

A later pass dropped both sides to 32 (Session 8's proven-safe number) purely
to make the two sides agree without guessing at unmeasured heap headroom.

Now raised to 128 - matching q3_ui/ui_servers2.c's own MAX_LISTBOXITEMS, i.e.
the actual number of rows the scroll-list widget can ever show at once, so
storage capacity equals display capacity exactly. Hand-computed cost: ~24 KB
here (cls.globalServers + globalServerAddresses) plus ~87 KB in
ui_servers2.c's g_globalserverlist[6][128], against the ~2 MB of heap
headroom previously measured - negligible, unlike the ~1.15 MB the 1024 case
would have cost.

ui_servers2.c's MAX_GLOBALSERVERS is #define'd to this constant directly
(not a second literal) specifically so the two can never drift apart again -
see that file rather than hand-syncing a number here.
*/
#define MAX_GLOBAL_SERVERS        128

/*
cl_serverStatusList[MAX_SERVERSTATUSREQUESTS] (client/cl_main.c:152) is 8244
bytes per entry. Same story as above: 2 was a loopback-only number. 16 is
upstream's value and costs 129 KB, which is affordable; this one is a
request-in-flight count, not a result count, so there is nothing to gain by
trimming it further.
*/
#define MAX_SERVERSTATUSREQUESTS  16

/*
Upstream's master.quake3arena.com (qcommon/qcommon.h:260) has been dead for
years, so the internet browser resolves nothing without an override. The
#ifndef guard there means this force-included header wins.

update.quake3arena.com is dead the same way, but UPDATE_SERVER_NAME is guarded
with "#if !defined UPDATE_SERVER_NAME && !defined STANDALONE" and CL_RequestMotd
(client/cl_main.c:1517) resolves it at startup - a multi-second DNS stall on the
main thread. That one is switched off with "+set cl_motd 0" from
Sys_PSP_BuildBootCommandLine instead, so the constant is left alone.
*/
#define MASTER_SERVER_NAME        "master.ioquake3.org"

/*
s_knownSfx[MAX_SFX] (client/snd_dma.c:73) is 100 bytes per entry, 400 KB.
Upstream's 4096 is sized for MAX_SOUNDS (256) plus custom player sounds
across 64 clients; this port's sound pool holds ~70 seconds of 22 kHz audio
total, so it can never keep 4096 sounds resident. S_FindName raises a clean
Com_Error at the cap rather than corrupting anything.
*/
#define MAX_SFX                   512

/*
MAX_DRAWSURFS (renderergl1/tr_local.h:789) is paid twice: 512 KB of hunk in
backEndData_t and 512 KB of .bss in R_RadixSort's scratch array. The Session 7
hardware run logged the whole of Q3DM1 as "1942 faces, 113 meshes, 42
trisurfs", i.e. ~2100 surfaces in the entire map before culling. 8192 is four
times the worst case, and R_SortDrawSurfs clamps rather than overruns.
*/
#define MAX_DRAWSURFS             0x2000

/*
edgeDefs[SHADER_MAX_VERTEXES][MAX_EDGE_DEFS] (renderergl1/tr_shadows.c:44) is
256 KB and feeds stencil shadow volumes only. This build has no stencil bits
at all - the Session 8 log's own renderer banner says "stencil(0-bits)",
because the framebuffer is 16-bit 5650 - so RB_ShadowTessEnd cannot produce
anything visible whatever this value is. cg_shadows 1 (blob shadows) is a
shader and does not come through here.
*/
#define MAX_EDGE_DEFS             4

/*
===========================================================================
Sys_PSP_HeapReport - heap and pak-handle instrumentation.

Written for the Session 7 map-load failure (unzOpen returning NULL for a pak
opened seconds earlier - the fix is the handle cache in psp_file.c) and kept,
because Session 8 needed it again for a completely different question and
Session 10's memory budget is nothing but these numbers.

Call sites: qcommon/common.c (end of Com_Init), qcommon/files.c (unzOpen
failure), server/sv_init.c (four points across a map load).

Read the mallinfo figures carefully. `arena` is how much of the
PSP_HEAP_SIZE_KB block newlib has actually sbrk'd so far, NOT the block - so
"free 57 KB" means 57 KB left inside the current arena, with the rest of the
block still available for the next extension. The real headroom is
PSP_HEAP_KB minus peak arena: the Session 8 run peaked at 37,907 KB of 39,936,
i.e. ~2 MB. Reading `free` as the whole story would have looked like an
emergency in a run where no allocation failed.
===========================================================================
*/
void Sys_PSP_HeapReport( const char *where );

/*
===========================================================================
Sys_PSP_Zone* - Session 10 step 1b frame breakdown.

Step 1a measured S_PaintChannels at 2-14% of wall time (~470 us per active
channel, ~9% typical) and, far more importantly, an S_Update_ interval of
78-173 ms: the port runs at 6-12 FPS. Mixing is therefore not the problem,
and the question that decides every remaining optimisation - including
whether the Media Engine is worth wiring up at all - is where the other ~90%
of a 125 ms frame goes.

Two candidates the log cannot separate:
  - the interpreted cgame QVM ("Architecture doesn't have a bytecode
    compiler"), which can NEVER be offloaded to the ME because QVM opcodes
    trap back into engine syscalls
  - the renderer backend / GE submission, which is exactly what the ME
    gu-shared-list pattern targets

ZONE_CGAME and ZONE_ENDFRAME are non-overlapping; ZONE_SVFRAME is the
listen-server tick. Whatever is left of the frame after the three is
reported as "other" (event loop, client prediction, sound paint, VM
translation, file I/O).

Note ZONE_ENDFRAME includes GLimp_EndFrame's vblank wait. At 8 FPS that wait
is ~0, but if this instrumentation outlives the current frame rate the zone
stops meaning "GPU work".

Remove all of this once the answer is in - it is a measurement, not a
feature.
===========================================================================
*/
#define PSP_ZONE_CGAME      0
#define PSP_ZONE_ENDFRAME   1
#define PSP_ZONE_SVFRAME    2

// Zones below this index partition the frame and are what "other" is computed
// against. Zones at or above it are SUBSETS of one of them and are reported
// but never added to the accounted total - double-counting them would make
// "other" collapse to zero and hide whatever it is really holding.
#define PSP_ZONE_TOPLEVEL   3

/*
Session 11c: endFrame is 42-60% of the frame and says nothing about WHY. It
covers re.EndFrame, i.e. RB_ExecuteRenderCommands (CPU: tessellation, the
clipper, vertex building, GE command submission) followed by GLimp_EndFrame,
which is where the CPU can finally wait for the hardware. Splitting it tests
the renderer critical path in the sampled workload:

  geSync   sceGuFinish + sceGuSync  - residual wait for the GE to complete
           the submitted list; zero means the wait did not extend that frame,
           not that the GE was never busy or cannot bind another workload
  vblank   sceDisplayWaitVblankStart + sceGuSwapBuffers - refresh wait, which
           should be ~0 below 59.94 fps and would otherwise be silently
           charged to the GE
  CPU      endFrame minus the two, derived at print time - no instrumentation
           inside PSP_DrawElements, whose per-draw call count would make the
           timestamps themselves part of the measurement
*/
#define PSP_ZONE_GESYNC     3
#define PSP_ZONE_VBLANK     4
#define PSP_ZONE_COUNT      5

void Sys_PSP_ZoneBegin( int zone );
void Sys_PSP_ZoneEnd( int zone );
void Sys_PSP_FrameMark( void );

/*
 Session 12 measurement-only renderer profile.  These scopes are sampled one
 renderer frame in sixteen, so the per-draw PSP_DrawElements breakdown does
 not become a permanent cost.  All scopes below are timed except drawScan,
 drawSubmit, fastTc, and surfFace, which are counted without timestamps; all
 four are covered by the timed backendSurfs scope.  Values are independent
 timings: a caller scope may contain another scope, therefore their reported
 shares must not be added.
*/
#define PSP_RPROF_NORMALIZE     0
#define PSP_RPROF_MD3LERP       1
#define PSP_RPROF_ENVTC         2
#define PSP_RPROF_SCALETC       3
#define PSP_RPROF_TRANSFORMTC   4
#define PSP_RPROF_DIFFUSE       5
#define PSP_RPROF_DRAW_SCAN     6
#define PSP_RPROF_DRAW_PACK     7
#define PSP_RPROF_DRAW_INDEX    8
#define PSP_RPROF_DRAW_OUTCODE  9
#define PSP_RPROF_DRAW_CLASSIFY 10
#define PSP_RPROF_DRAW_CLIP     11
#define PSP_RPROF_DRAW_SUBMIT   12
/*
 Session 12b.  Stages psp_tcmod.c claimed, i.e. that skipped ComputeTexCoords'
 per-vertex texture-coordinate copy entirely.  Unlike every scope above it this
 one is per STAGE, not per vertex, so it is counted without a timestamp; its
 value is the CALL COUNT, which says how much of the frame took the fast path.
*/
#define PSP_RPROF_FASTTC        13
/*
 Session 13b.  These scopes attribute the frame-time remainder before any
 further optimization is selected.  They deliberately overlap: report each
 independently and do not sum their shares.
*/
#define PSP_RPROF_WORLD         14
#define PSP_RPROF_DEFORM        15
#define PSP_RPROF_SURF_TRI      16
#define PSP_RPROF_SURF_FACE     17
#define PSP_RPROF_SURF_GRID     18
#define PSP_RPROF_COLORS        19
#define PSP_RPROF_DLIGHT        20
#define PSP_RPROF_FOG           21
#define PSP_RPROF_CLIENT_FRAME  22
#define PSP_RPROF_SOUND_UPDATE  23
/* Session 13c: nested inside sound.  soundRest is derived from sound minus
   soundPaint in the report so no extra timestamps disturb the hot path. */
#define PSP_RPROF_SOUND_PAINT   24
/* Session 15: split the native cgame call into its CPU-side stages. */
#define PSP_RPROF_CGAME_SIM     25
#define PSP_RPROF_CGAME_ENTS    26
#define PSP_RPROF_CGAME_WEAPON  27
#define PSP_RPROF_CGAME_DRAW    28
#define PSP_RPROF_BACKEND_CMDS  29
#define PSP_RPROF_BACKEND_SURFS 30
/* Session 16: disjoint native-cgame attribution and syscall bridge timing. */
#define PSP_RPROF_CGAME_SNAPSHOT 31
#define PSP_RPROF_CGAME_PREDICT   32
#define PSP_RPROF_CGAME_VIEW      33
#define PSP_RPROF_CGAME_PACKET    34
#define PSP_RPROF_CGAME_MARKS     35
#define PSP_RPROF_CGAME_PARTICLES 36
#define PSP_RPROF_CGAME_LOCAL     37
#define PSP_RPROF_CGAME_SCENE     38
#define PSP_RPROF_CGAME_HUD       39
#define PSP_RPROF_SOUND_LAZY_LOAD 40
#define PSP_RPROF_SOUND_EVICT     41
#define PSP_RPROF_SOUND_CODEC     42
#define PSP_RPROF_SOUND_RESAMPLE  43
#define PSP_RPROF_SOUND_CODEC_OPEN 44
#define PSP_RPROF_SOUND_CODEC_HEADER 45
#define PSP_RPROF_SOUND_CODEC_ALLOC 46
#define PSP_RPROF_SOUND_CODEC_READ 47
#define PSP_RPROF_SOUND_PCM_ALLOC 48
#define PSP_RPROF_SOUND_POOL_ALLOC 49
#define PSP_RPROF_COUNT           50

/*
 Goal 22 file-read stall attribution.  These are independent operation
 categories: parent operations intentionally overlap their child stages.
 The sink aggregates every event and retains only slow (>=1 ms) window tails.
*/
#define PSP_TRACE_VF_OPEN             0
#define PSP_TRACE_VF_READ             1
#define PSP_TRACE_VF_SEEK             2
#define PSP_TRACE_VF_EVICT            3
#define PSP_TRACE_VF_RESTORE          4
#define PSP_TRACE_VF_RESTORE_OPEN    5
#define PSP_TRACE_VF_RESTORE_SEEK    6
#define PSP_TRACE_SOUND_LAZY_LOAD    7
#define PSP_TRACE_SOUND_CODEC        8
#define PSP_TRACE_SOUND_CODEC_OPEN   9
#define PSP_TRACE_SOUND_CODEC_HEADER 10
#define PSP_TRACE_SOUND_CODEC_ALLOC  11
#define PSP_TRACE_SOUND_CODEC_READ   12
#define PSP_TRACE_SOUND_PCM_ALLOC    13
#define PSP_TRACE_SOUND_RESAMPLE     14
#define PSP_TRACE_NET_SELECT         15
#define PSP_TRACE_NET_DELAY          16
#define PSP_TRACE_COUNT              17

#ifdef PSP_STUTTER_TRACE
/*
 * Goal 23 stutter attribution.  These records are emitted only for complete
 * operations and slow events; all calls still contribute to the aggregate
 * counters.  The enum is intentionally stable because the post-workload
 * trace is consumed outside the PSP.
 */
#define PSP_STUTTER_PHASE_NONE              0
#define PSP_STUTTER_PHASE_LOOKUP_TOTAL      1
#define PSP_STUTTER_PHASE_LOOSE_STAT        2
#define PSP_STUTTER_PHASE_LOOSE_FOPEN       3
#define PSP_STUTTER_PHASE_PACK_HASH         4
#define PSP_STUTTER_PHASE_UNZ_OFFSET        5
#define PSP_STUTTER_PHASE_LOCAL_HEADER      6
#define PSP_STUTTER_PHASE_REFILL_SEEK       7
#define PSP_STUTTER_PHASE_REFILL_READ       8
#define PSP_STUTTER_PHASE_VF_OPEN           9
#define PSP_STUTTER_PHASE_VF_READ          10
#define PSP_STUTTER_PHASE_VF_SEEK          11
#define PSP_STUTTER_PHASE_VF_EVICT         12
#define PSP_STUTTER_PHASE_VF_RESTORE       13
#define PSP_STUTTER_PHASE_RESTORE_OPEN     14
#define PSP_STUTTER_PHASE_RESTORE_SEEK     15
#define PSP_STUTTER_PHASE_COUNT            16

#define PSP_STUTTER_SOURCE_MISS             0
#define PSP_STUTTER_SOURCE_LOOSE            1
#define PSP_STUTTER_SOURCE_PACK             2

#define PSP_STUTTER_FLAG_RESTORE            1
#define PSP_STUTTER_FLAG_EVICT              2
#define PSP_STUTTER_FLAG_PARENT             4
#define PSP_STUTTER_FLAG_CHILD              8

unsigned int Sys_PSP_StutterTraceHashPath( const char *path );
void Sys_PSP_StutterTraceLookupBegin( const char *qpath );
void Sys_PSP_StutterTraceLookupSource( int source );
void Sys_PSP_StutterTraceLookupEnd( int result );
void Sys_PSP_StutterTraceSetContext( const char *qpath,
	unsigned int packHash );
void Sys_PSP_StutterTraceClearContext( void );
void Sys_PSP_StutterTraceSetPack( const char *packPath );
void Sys_PSP_StutterTraceSetPhase( int phase );
int Sys_PSP_StutterTraceCurrentPhase( int fallback );
void Sys_PSP_StutterTraceRecord( int phase, unsigned int elapsedUs,
	int logicalHandle, int physicalSlot, int offset, int origin,
	int savedCursor, int result, unsigned int flags, const char *packPath );
void Sys_PSP_StutterTraceDump( void );
#endif

unsigned int Sys_PSP_RenderProfileNow( void );

#ifdef PSP_RENDER_PROFILE
void Sys_PSP_RenderProfileFrameBegin( void );
void Sys_PSP_RenderProfileBegin( int scope );
void Sys_PSP_RenderProfileEnd( int scope );
void Sys_PSP_RenderProfileCount( int scope );
void Sys_PSP_RenderProfileSoundAsset( const char *name );
void Sys_PSP_RenderProfileSoundLoad( const char *name, int rate, int width,
	int channels, int bytes, int loadCount );
unsigned int Sys_PSP_RenderProfileCGameSyscallBegin( int callNum );
void Sys_PSP_RenderProfileCGameSyscallEnd( int callNum, unsigned int start );
#else
#define Sys_PSP_RenderProfileFrameBegin() ((void)0)
#define Sys_PSP_RenderProfileBegin( scope ) ((void)0)
#define Sys_PSP_RenderProfileEnd( scope ) ((void)0)
#define Sys_PSP_RenderProfileCount( scope ) ((void)0)
#define Sys_PSP_RenderProfileSoundAsset( name ) ((void)0)
#define Sys_PSP_RenderProfileSoundLoad( name, rate, width, channels, bytes, loadCount ) ((void)0)
#define Sys_PSP_RenderProfileCGameSyscallBegin( callNum ) ( 0U )
#define Sys_PSP_RenderProfileCGameSyscallEnd( callNum, start ) ((void)0)
#endif
void Sys_PSP_FileTraceFrameBegin( void );
void Sys_PSP_FileTraceEvent( int category, unsigned int elapsedUs,
	unsigned int value );

// Peak bytes taken from psp_draw.c's vertex arena, in KB. Defined there;
// declared here rather than in psp_draw.h so sys_psp.c can report it without
// including the renderer's tr_common.h.
int PSP_DrawArenaPeakKB( void );

void PSP_StaticWorld_ClassifySurface( const void *surface, const void *shader,
	int fogNum );
void PSP_StaticWorld_Reset( void );
void PSP_StaticWorld_Report( void );

/*
 Whether a UDP socket is currently bound. Defined in psp_net.c; declared here
 so psp_glimp.c can gate its per-frame yield on it without pulling in
 qcommon's networking prototypes. See r_pspNetVblank in psp_glimp.c.

 Plain int, not qboolean: this header is force-included into every translation
 unit from the command line (see cmake/platforms/psp.cmake), including the game
 modules, where q_shared.h has not been seen yet. Nothing else in this file uses
 an engine type either - keep it that way.
*/
int NET_PSP_IsSocketOpen( void );

/* Whether the client is currently connecting to or playing on a remote
   server. Defined in code/client/cl_main.c so the renderer can keep the
   network vblank rendezvous without treating the WLAN socket itself as
   proof that a remote game is active. */
int CL_PSP_IsRemoteSession( void );

#endif // __PSP_PLATFORM_H__
