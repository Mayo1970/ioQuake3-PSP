// Force-included into every translation unit (cmake/platforms/psp.cmake), game modules too.
// Budget floors for common.c's #ifndef guards; sys_psp.c injects the real com_hunkMegs at boot.

#ifndef __PSP_PLATFORM_H__
#define __PSP_PLATFORM_H__

#define DEF_COMHUNKMEGS   27
#define MIN_COMHUNKMEGS   15
#define DEF_COMZONEMEGS   5

// Streams above 0 are VoIP only and USE_VOIP is off; stream 0 carries music and cinematics.
// Upstream's 129 streams are 16.9 MB of .bss; one is 128 KB.
#define MAX_RAW_STREAMS   1

// sv_main.c rate-limit buckets, 40 bytes each; a PSP listen server holds a handful of players.
// MAX_HASHES stays 1024: SVC_HashForAddress masks with it independently of this count.
#define MAX_BUCKETS       256

// Session 8 .bss cuts: each sound unit costs ~3 MB, and these caps paid for it out of .bss.

// Rows of cls.globalServers, ~284 bytes each; equals q3_ui's MAX_LISTBOXITEMS.
// ui_servers2.c's MAX_GLOBALSERVERS is defined to this, so the two cannot drift apart.
#define MAX_GLOBAL_SERVERS        128

// Status requests in flight, 8244 bytes each; upstream's 16 is affordable.
#define MAX_SERVERSTATUSREQUESTS  16

// master.quake3arena.com is dead. The dead update server is disabled with cl_motd 0 instead.
// OpenArena servers list on dpmaster only, as in the OA engine.
#ifdef STANDALONEOA
#define MASTER_SERVER_NAME        "dpmaster.deathmask.net"
#else
#define MASTER_SERVER_NAME        "master.ioquake3.org"
#endif

// Team Arena runs from the q3 install folder, so its logs get their own names (Xbox: ioquake3_ta.log).
#ifdef MISSIONPACK
#define PSP_LOG_TAG               "_ta"
#else
#define PSP_LOG_TAG               ""
#endif

// s_knownSfx rows, 108 bytes each; the sound pool can never hold 4096 sounds resident.
// Team Arena also names its 796 voice chat sounds, which load on first play (snd_dma.c).
#ifdef MISSIONPACK
#define MAX_SFX                   1536
#else
#define MAX_SFX                   512
#endif

// Paid twice (hunk and R_RadixSort .bss). Q3DM1 has ~2100 surfaces; the sort clamps, it does not overrun.
#define MAX_DRAWSURFS             0x2000

// Stencil shadow volume edges only; the 5650 framebuffer has no stencil bits.
#define MAX_EDGE_DEFS             4

// R_RegisterMD3 loads only this LOD (or the next better one); LOD1 halves a player model.
#define PSP_MD3_LOD               1

// bot_thinktime default in ms for sv_bot.c and ai_main.c (upstream 100). Still cheat-protected.
#define PSP_BOT_THINKTIME         "150"

// Area routing caches one server frame may build (be_aas_route.c). A first route can need ~130.
#define PSP_AAS_FRAME_ROUTING_BUILDS 6

// XMB nickname for the "name" cvar default, "UnnamedPlayer" when unset. Never NULL.
const char *Sys_PSP_DefaultPlayerName( void );

// Heap, hunk, zone, sound and pak-handle report. mallinfo "free" is inside the arena only:
// the real heap headroom is PSP_HEAP_KB minus the peak arena.
void Sys_PSP_HeapReport( const char *where );

// +1 while a hunk temp block not from FS_ReadFile is alive, -1 after it is freed (files.c).
void FS_PSP_HoldTempMemory( int delta );

// Free bytes now and lowest since the last call, or -1 before the pool exists.
void Z_PSP_FreeMemory( int *freeBytes, int *lowestFree, int *largestFree );
#ifdef PSP_XBOX_MEMORY
// Largest free main-zone block in bytes (block header included), 0 before the zone exists.
int Z_PSP_LargestFree( void );
// Nonzero when ptr is main-zone memory; plain int, since game modules see no qboolean here.
int Z_PSP_InMainZone( const void *ptr );
#endif
void SND_PSP_FreeMemory( int *freeBytes, int *lowestFree );

// Texture memory by pool; defined in psp_tex.c, declared here for cl_cgame.c.
void PSP_TexMemReport( void );

// ADPCM decode cache hits and decodes per frame-report window (psp_adpcm.c).
void PSP_AdpcmCacheReport( void );

// Frame breakdown: CGAME, ENDFRAME and SVFRAME partition the frame; the rest is "other".
// ENDFRAME includes the vblank wait, so it stops meaning GPU work near 60 FPS.
#define PSP_ZONE_CGAME      0
#define PSP_ZONE_ENDFRAME   1
#define PSP_ZONE_SVFRAME    2

// Zones from here on are subsets of a top-level zone and are never added to the total.
#define PSP_ZONE_TOPLEVEL   3

// geSync: residual wait for the GE list; vblank: refresh wait. CPU = endFrame minus both,
// derived at print time so PSP_DrawElements carries no timestamps.
#define PSP_ZONE_GESYNC     3
#define PSP_ZONE_VBLANK     4
#define PSP_ZONE_COUNT      5

void Sys_PSP_ZoneBegin( int zone );
void Sys_PSP_ZoneEnd( int zone );
void Sys_PSP_FrameMark( void );

// Renderer profile, sampled one frame in four. drawScan, drawSubmit, fastTc and surfFace
// are counted, not timed. Scopes may nest, so their shares must not be added.
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
// Per stage, not per vertex: the call count of stages psp_tcmod.c took the fast path for.
#define PSP_RPROF_FASTTC        13
// Session 13b remainder attribution; these overlap, so report each on its own.
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
// Nested inside sound; soundRest is derived as sound minus soundPaint.
#define PSP_RPROF_SOUND_PAINT   24
// Session 15: the native cgame call split into its CPU-side stages.
#define PSP_RPROF_CGAME_SIM     25
#define PSP_RPROF_CGAME_ENTS    26
#define PSP_RPROF_CGAME_WEAPON  27
#define PSP_RPROF_CGAME_DRAW    28
#define PSP_RPROF_BACKEND_CMDS  29
#define PSP_RPROF_BACKEND_SURFS 30
// Session 16: disjoint native-cgame attribution and syscall bridge timing.
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

// Goal 22 file-read stall categories; parents overlap their child stages.
// The sink aggregates every event and keeps only slow (>=1 ms) window tails.
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
// Goal 23 stutter records, emitted for complete slow operations only.
// The values are stable because the trace is decoded off the PSP.
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
// One shader batch, RB_BeginSurface to RB_EndSurface; Begin returns 0 outside sampled frames.
unsigned int Sys_PSP_RenderProfileBatchBegin( void );
void Sys_PSP_RenderProfileBatchEnd( unsigned int start, const char *shader, int numVertexes );
#else
#define Sys_PSP_RenderProfileFrameBegin() ((void)0)
#define Sys_PSP_RenderProfileBegin( scope ) ((void)0)
#define Sys_PSP_RenderProfileEnd( scope ) ((void)0)
#define Sys_PSP_RenderProfileCount( scope ) ((void)0)
#define Sys_PSP_RenderProfileSoundAsset( name ) ((void)0)
#define Sys_PSP_RenderProfileSoundLoad( name, rate, width, channels, bytes, loadCount ) ((void)0)
#define Sys_PSP_RenderProfileCGameSyscallBegin( callNum ) ( 0U )
#define Sys_PSP_RenderProfileCGameSyscallEnd( callNum, start ) ((void)0)
#define Sys_PSP_RenderProfileBatchBegin() ( 0U )
#define Sys_PSP_RenderProfileBatchEnd( start, shader, numVertexes ) ((void)(start))
#endif

// Unsampled counters for load and server work; each frame and heap report prints and resets them.
// Debug only: SV_Trace alone makes hundreds of timed calls per fight frame.
#define PSP_COUNT_FS_LOOKUP       0
#define PSP_COUNT_FS_MISS         1
#define PSP_COUNT_FS_LOOSE        2
// Loose-folder checks skipped because the qpath's top folder is not in that folder (files.c).
#define PSP_COUNT_FS_LOOSE_SKIP   3
#define PSP_COUNT_FS_PACKOPEN     4
// New pk3 handles (unzOpen), used when a lookup needs its own cursor.
#define PSP_COUNT_FS_UNZOPEN      5
#define PSP_COUNT_FS_READ         6
#define PSP_COUNT_IMAGE_LOAD      7
#define PSP_COUNT_IMAGE_CREATE    8
#define PSP_COUNT_DXT             9
#define PSP_COUNT_WORLD           10
#define PSP_COUNT_SV_BOTS         11
#define PSP_COUNT_SV_GAME         12
#define PSP_COUNT_SV_SNAP         13
#define PSP_COUNT_SV_TRACE        14
#define PSP_COUNT_AAS_AREACACHE   15
#define PSP_COUNT_AAS_PORTALCACHE 16
#define PSP_COUNT_AAS_FREE_MEM    17
#define PSP_COUNT_AAS_FREE_CAP    18
// Bot file loads through the game syscalls (sv_game.c): character, chat, item and weapon weights.
#define PSP_COUNT_BOT_CHAR        19
#define PSP_COUNT_BOT_CHAT        20
#define PSP_COUNT_BOT_WEIGHT      21
// Route queries refused because the server frame's routing-cache build budget is spent.
#define PSP_COUNT_AAS_BUDGET      22
#define PSP_COUNT_COUNT           23

#if defined( PSP_RENDER_PROFILE ) && !defined( NDEBUG )
#define PSP_COUNTERS 1
unsigned int Sys_PSP_CountBegin( void );
void Sys_PSP_CountEnd( int counter, unsigned int start, unsigned int value );
void Sys_PSP_CountEvent( int counter );
void Sys_PSP_CountMiss( const char *qpath );
void Sys_PSP_CountReport( const char *where, unsigned int frameCount );
#else
#define Sys_PSP_CountBegin() ( 0U )
#define Sys_PSP_CountEnd( counter, start, value ) ((void)(start))
#define Sys_PSP_CountEvent( counter ) ((void)0)
#define Sys_PSP_CountMiss( qpath ) ((void)0)
#define Sys_PSP_CountReport( where, frameCount ) ((void)0)
#endif
void Sys_PSP_FileTraceFrameBegin( void );
void Sys_PSP_FileTraceEvent( int category, unsigned int elapsedUs,
	unsigned int value );

// Vertex arena peak in KB, declared here so sys_psp.c need not include tr_common.h.
int PSP_DrawArenaPeakKB( void );

void PSP_StaticWorld_ClassifySurface( const void *surface, const void *shader,
	int fogNum );
void PSP_StaticWorld_Reset( void );
void PSP_StaticWorld_Report( void );

// Whether a UDP socket is bound (psp_net.c). Plain int, not qboolean: game modules include
// this header before q_shared.h, so nothing here may use an engine type.
int NET_PSP_IsSocketOpen( void );

// Whether the client is connecting to or playing on a remote server (cl_main.c).
int CL_PSP_IsRemoteSession( void );

// Resets the routing-cache build budget (be_aas_route.c); SV_BotFrame calls it every server frame.
void AAS_PSP_BeginServerFrame( void );

#endif // __PSP_PLATFORM_H__
