/*
===========================================================================
PSP port - code/psp/psp_glimp.c

Session 5. Owns the display: the Slim gate, VRAM framebuffers, sceGuInit,
the display list, and the swap. The qgl* vtable moved to psp_qgl.c.

The reference for every value here is Quake3PSP-mirror/unix/glimp.cpp
(Crow_bar's port - the only Quake 3 known to run on real PSP hardware),
with three deliberate departures, all recorded in Q3PORT.md Session 5:

  - the vblank wait is CONDITIONAL, and there are three colour buffers
    rather than two (Session 11d). The mirror skips the wait when
    net_connected (glimp.cpp:93-94) and tears for it. Here the third
    buffer removes the reason to tear: the flip is still queued for a
    refresh boundary, and the wait only happens when the engine is running
    ahead of the panel. Session 11c measured the unconditional wait at
    7-10 ms of every frame - see PSP_NUM_FRAMEBUFFERS below.
  - the display list is heap memory, not a 1 MB .bss array
    (glimp.cpp:70). Large .bss has a demonstrated history of breaking
    module load in this tree - see the BUILD_PRX note in
    cmake/platforms/psp.cmake.
  - sceGeEdramSetSize(4 MB) is attempted before the first vramalloc. It
    FAILS on this firmware from user mode - measured 0x8002013A,
    SCE_KERNEL_ERROR_LIBRARY_NOT_YET_LINKED, on a PSP-2000 - so the
    aperture stays at 2 MB. The call is kept and its result logged
    because the allocator sizes itself from sceGeEdramGetSize(), so a
    machine where it does resolve gets the extra 2 MB for free. The
    mirror never calls it at all.

The 5650 framebuffer is the mirror's shipping configuration
(glimp.cpp:34 "#define DP565"). It halves framebuffer bandwidth, which is
what the GE is actually short of. It has no destination alpha, so
glConfig.stencilBits is 0 and ioquake3 drops to its non-stencil shadow
path on its own.

What is left over after the three buffers measured 1.23 MB on hardware,
which is why Session 6's textures live in main RAM (see psp_tex.h) and
why which textures earn VRAM is a Session 11 decision made against a
framerate rather than a guess made here.
===========================================================================
*/

#include "../renderercommon/tr_common.h"
#include "../sys/sys_local.h"
#include "psp_gu.h"
#include "psp_draw.h"
#include "psp_tex.h"
#include "psp_pool.h"

#include <pspdisplay.h>
#include <pspge.h>
#include <kubridge.h>
#include <vram.h>

#include <malloc.h>
#include <string.h>

/*
 Command words only, as of Session 10.

 Sessions 5-7 sized this against vertex data too, because psp_draw.c built
 every interleaved batch with sceGuGetMemory, which carves out of this same
 buffer - 1 MB, then 2 MB. Hardware said that was still not enough
 ("display list full, 42 draws skipped") and the frame breakdown said the
 uncached mirror this buffer lives behind was a large part of why
 re.EndFrame cost ~45% of the frame.

 psp_draw.c now owns a separate cached arena for data and calls
 sceGuGetMemory nowhere, so this only has to hold GE commands. A world frame
 issues hundreds of batches, each preceded by state changes of a few dozen
 words; 512 KB is roughly an order of magnitude more than that needs and
 still returns 1.5 MB to the heap versus Session 7.

 There is no bounds check on the command side - libpspgu just writes - so if
 a frame ever overruns this the symptom is heap corruption, not a message.
 That is unchanged from every previous session; only the size moved.
*/
#define PSP_DISPLAY_LIST_BYTES	( 512 * 1024 )

/*
 Clear the colour buffer at the top of every frame. OFF as of Session 6.

 ioquake3 does NOT clear colour by default: RB_DrawBuffer only clears when
 r_clear is set (tr_backend.c:942), because on every other platform the 2D
 pass is guaranteed to cover the screen. Session 5 could not rely on that -
 every draw entry point was a no-op, so nothing wrote colour and the swap
 chain alternated two buffers of stale VRAM, which is why a working swap and
 a broken one looked identical. Clearing made the gate observable.

 BACK ON for the Session 6 diagnostic, and the reason is worth recording:
 with it off, the screen kept showing the pspDebugScreen boot text, which
 writes VRAM at offset 0 as 8888 while the GU scans the same memory out as
 5650 at stride 512. Thirty-two-bit pixels read as sixteen come out
 horizontally doubled and colour-garbled, which reads to the eye as
 "the text is huge and stretched" - and looks exactly like a broken draw
 path while actually meaning NOTHING was drawn over it. Two rounds were
 spent on that.

 A clear that runs every frame makes the two states tell themselves apart:
 a dark blue screen means frames are running and the engine issued no draws,
 stale boot text means frames are not running at all.

 Session 7 deletes it, once the 2D pass is confirmed to cover the screen.
*/
/*
 Session 7: this became the cvar r_pspClear, default 0, and the #define is
 gone.

 The world pass now covers the screen, so the clear is pure wasted fill rate
 on a GPU that has none to spare - which is why it defaults off. But it stays
 reachable, because it is the one thing that tells "frames are running and
 nothing drew" apart from "frames are not running at all", and losing that
 distinction cost Session 6 two hardware rounds.

 It is a cvar rather than a rebuild because it is a runtime renderer
 diagnostic. Text entry is not part of this session.
*/
static cvar_t	*r_pspClear;
static cvar_t	*r_pspNetVblank;

qboolean			pspGuReady = qfalse;

/*
 THREE colour buffers, not two (Session 11d).

 Session 11c measured the vblank wait at 7-10 ms of every frame - 8-18% of a
 gameplay frame and 53% of a menu frame - spent aligning to a 59.94 Hz beat
 this port is nowhere near. It cannot simply be deleted with double
 buffering: sceGuSwapBuffers queues the flip for the NEXT vblank
 (PSP_DISPLAY_SETBUF_NEXTFRAME), so without the wait the engine starts
 drawing into a buffer the panel is still scanning out, for up to 16.7 ms.

 With three, the buffer we draw into is neither the one being displayed nor
 the one queued, so the wait is only needed when the engine is running FASTER
 than the panel - which is what the vcount check in GLimp_EndFrame tests.
 Cost is one more 272 KB buffer of VRAM. Session 11c's geSync ~= 0 ms means
 this extra allocation did not lengthen the sampled gameplay frames; it does
 not establish that the GE is never busy or that this VRAM cannot matter to
 another workload. See the scoped result in Q3PORT.md.
*/
#define PSP_NUM_FRAMEBUFFERS	3

static unsigned short	*pspFrameBuffer[ PSP_NUM_FRAMEBUFFERS ];
static int		pspDrawIndex;		// GE renders into this one
static unsigned int	pspLastFlipVcount;	// panel vcount at the last flip
static unsigned short	*pspDepthBuffer;
static unsigned int		*pspDisplayList;

/*
===============
GLimp_BeginList

Opens the display list for a frame. See PSP_GU_CLEAR_EACH_FRAME above for
why the clear is here and what removes it.

The colour is the mirror's (unix/glimp.cpp:154) - a dark blue that no
failure mode produces by accident, so "the screen is this colour" and "the
screen is black" stay distinguishable. It is set every frame rather than
once at init because RB_Hyperspace and the fast-sky path both call
qglClearColor and never restore it.
===============
*/
static void GLimp_BeginList( void )
{
	sceGuStart( GU_DIRECT, pspDisplayList );

	// Which of the three buffers this frame renders into. sceGuDrawBufferList
	// is the in-list form and must follow sceGuStart; sceGuDrawBuffer would
	// flush a list of its own.
	sceGuDrawBufferList( GU_PSM_5650, vrelptr( pspFrameBuffer[ pspDrawIndex ] ),
		PSP_BUFFER_STRIDE );

	/*
	 sceGuStart just reset the list, so the running total of what
	 sceGuGetMemory has taken out of it resets here too. psp_draw.c has to
	 track that itself: sceGuGetMemory performs no bounds check and never
	 returns NULL (pspsdk-src/src/gu/sceGuGetMemory.c), so overrunning the
	 list would scribble past this 1 MB allocation into the heap.
	*/
	PSP_DrawBeginFrame();

	if( r_pspClear && r_pspClear->integer )
	{
		sceGuClearColor( GU_RGBA( 0x10, 0x20, 0x40, 0xFF ) );
		sceGuClearDepth( 65535 );
		sceGuClear( GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT );
	}
}

/*
===============
GLimp_Shutdown
===============
*/
void GLimp_Shutdown( void )
{
	if( pspGuReady )
	{
		sceGuFinish();
		sceGuSync( 0, 0 );
		sceGuTerm();

		pspGuReady = qfalse;
		CON_SetDisplayOwnedByGU( qfalse );
	}

	PSP_DrawShutdownArena();

	if( pspDepthBuffer )
	{
		vfree( pspDepthBuffer );
		pspDepthBuffer = NULL;
	}
	{
		int	i;

		for( i = 0; i < PSP_NUM_FRAMEBUFFERS; i++ )
		{
			if( pspFrameBuffer[ i ] )
			{
				vfree( pspFrameBuffer[ i ] );
				pspFrameBuffer[ i ] = NULL;
			}
		}
	}
	if( pspDisplayList )
	{
		free( pspDisplayList );
		pspDisplayList = NULL;
	}

	memset( &glConfig, 0, sizeof( glConfig ) );
}

/*
===============
GLimp_Minimize
===============
*/
void GLimp_Minimize( void )
{
}

/*
===============
GLimp_LogComment
===============
*/
void GLimp_LogComment( char *comment )
{
}

/*
===============
GLimp_ClaimVRAM

sceGeEdramSetSize MUST run before the first vramalloc. libpspvram sizes its
heap from sceGeEdramGetSize() the first time it is asked for memory, so
raising the aperture afterwards has no effect on the allocator and the
extra 2 MB stays unreachable. Raising it and then allocating past 2 MB
without having raised it is the "corrupted display on a Slim" failure in
Q3PORT.md 5.

DECISION-004: this port is Slim (PSP-2000) and later only. kuKernelGetModel
returns 0 for a PSP-1000. Refusing here rather than in Sys_PlatformInit is
deliberate - see the note at code/sys/sys_psp.c:577-580.
===============
*/
static void GLimp_ClaimVRAM( void )
{
	const int	model = kuKernelGetModel();
	const int	bufferBytes = PSP_BUFFER_STRIDE * PSP_SCREEN_HEIGHT * (int)sizeof( unsigned short );
	int		edramResult;
	int		i;

	if( model == 0 )
	{
		ri.Error( ERR_FATAL,
			"This port needs a PSP-2000 or later (64 MB RAM, 4 MB VRAM).\n"
			"kuKernelGetModel() reports a PSP-1000." );
		return;
	}

	/*
	 Log the return code, not just the resulting size. The first hardware run
	 reported model 1 (a Slim) and an aperture still at 2 MB, which says the
	 call was reached and refused - and refused for a reason only the return
	 code names. sceGeEdramSetSize is a sceGe_user export (verified: the stub
	 lives in libpspge.a's sceGe_user_0019.o), so this is a real syscall
	 returning a real error, not an unresolved import.

	 Nothing here depends on the call succeeding: the allocator sizes itself
	 from whatever sceGeEdramGetSize reports, so a 2 MB aperture just means a
	 smaller Session 6 texture heap. Do not gate the display on it.
	*/
	edramResult = sceGeEdramSetSize( 4 * 1024 * 1024 );

	ri.Printf( PRINT_ALL, "PSP model %d, sceGeEdramSetSize(4MB) -> 0x%08X, GE eDRAM %u bytes at %p\n",
		model, (unsigned int)edramResult, sceGeEdramGetSize(), sceGeEdramGetAddr() );

	for( i = 0; i < PSP_NUM_FRAMEBUFFERS; i++ )
		pspFrameBuffer[ i ] = (unsigned short *)vramalloc( bufferBytes );

	pspDepthBuffer = (unsigned short *)vramalloc( bufferBytes );

	if( !pspFrameBuffer[ 0 ] || !pspFrameBuffer[ 1 ] || !pspFrameBuffer[ 2 ] ||
		!pspDepthBuffer )
	{
		ri.Error( ERR_FATAL, "GLimp_Init: out of VRAM (%d bytes x%d, %u free)",
			bufferBytes, PSP_NUM_FRAMEBUFFERS + 1, (unsigned int)vmemavail() );
		return;
	}

	pspDrawIndex = 0;

	ri.Printf( PRINT_ALL, "VRAM: frame %p %p %p, depth %p, %u free for textures\n",
		pspFrameBuffer[ 0 ], pspFrameBuffer[ 1 ], pspFrameBuffer[ 2 ],
		pspDepthBuffer, (unsigned int)vmemavail() );

	/*
	 The volatile partition is claimed here, before a single texture exists,
	 because psp_tex.c prefers it over the heap for every mip level. Failure
	 is not fatal: PSP_PoolAlloc then returns NULL and textures fall back to
	 memalign, i.e. exactly the Session 11c behaviour this replaces.
	*/
	PSP_PoolInit();
}

/*
===============
GLimp_InitGU

Mirrors Quake3PSP-mirror/unix/glimp.cpp:127-190.

Depth is NOT inverted: sceGuDepthRange(0, 65535) with GU_LEQUAL. That is
what the mirror ships (glimp.cpp:157-158) and it makes the
qglDepthFunc(GL_LEQUAL) ioquake3 actually sets a 1:1 translation. Q3PORT.md
Session 5 called for the inverted convention; inverting would force
psp_qgl.c to flip every depth comparison and every qglDepthRange for no
gain. Corrected there.
===============
*/
static void GLimp_InitGU( void )
{
	sceGuInit();

	sceGuStart( GU_DIRECT, pspDisplayList );

	// Draw into [0], show [2]: the rotation in GLimp_EndFrame then advances
	// 0 -> 1 -> 2 -> 0 with the displayed buffer always two steps behind.
	sceGuDrawBuffer( GU_PSM_5650, vrelptr( pspFrameBuffer[ 0 ] ), PSP_BUFFER_STRIDE );
	sceGuDispBuffer( PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT,
		vrelptr( pspFrameBuffer[ PSP_NUM_FRAMEBUFFERS - 1 ] ), PSP_BUFFER_STRIDE );
	sceGuDepthBuffer( vrelptr( pspDepthBuffer ), PSP_BUFFER_STRIDE );

	// The GE renders into a 4096x4096 virtual space; this centres the panel
	// in it. psp_qgl.c's qglViewport/qglScissor work off the same origin.
	sceGuOffset( 2048 - ( PSP_SCREEN_WIDTH / 2 ), 2048 - ( PSP_SCREEN_HEIGHT / 2 ) );
	sceGuViewport( 2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT );

	sceGuScissor( 0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT );
	sceGuEnable( GU_SCISSOR_TEST );

	sceGuDepthRange( 0, 65535 );
	sceGuDepthFunc( GU_LEQUAL );
	sceGuEnable( GU_DEPTH_TEST );
	sceGuDepthMask( GU_FALSE );	// GU_FALSE == writes ENABLED, inverted vs GL

	sceGuClearDepth( 65535 );
	sceGuClearColor( 0 );

	sceGumMatrixMode( GU_PROJECTION );
	sceGumLoadIdentity();
	sceGumUpdateMatrix();
	// GU_VIEW is pinned to identity for the life of the process: GL has one
	// modelview matrix, so psp_qgl.c maps GL_MODELVIEW onto GU_MODEL alone.
	sceGumMatrixMode( GU_VIEW );
	sceGumLoadIdentity();
	sceGumUpdateMatrix();
	sceGumMatrixMode( GU_MODEL );
	sceGumLoadIdentity();
	sceGumUpdateMatrix();

	sceGuFrontFace( GU_CCW );
	sceGuEnable( GU_CULL_FACE );

	// The GE has no near-plane clipping. GU_CLIP_PLANES is the frustum
	// reject it does have; the CPU near-plane clipper is Session 7.
	sceGuEnable( GU_CLIP_PLANES );

	sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
	sceGuEnable( GU_TEXTURE_2D );

	/*
	 MANDATORY, and the reason the first Session 6 hardware run drew text as
	 huge smeared blocks.

	 sceGuInit's init list zeroes the texture scale registers -
	 pspsdk-src/src/gu/sceGuInit.c:75-78 is ZV(TEX_SCALE_U)/ZV(TEX_SCALE_V),
	 and ZV(command) is (command << 24 | 0x00000000). So the GE starts with a
	 texture scale of 0.0, not 1.0. The 3D T&L pipe multiplies every incoming
	 UV by it, so all texture coordinates collapse to (0,0) and each triangle
	 samples a single texel of its texture stretched across the whole
	 primitive.

	 Quake3PSP-mirror never needed this because it draws 2D with
	 GU_TRANSFORM_2D, which pspgu.h:1362-1364 states is not affected by
	 sceGuTexOffset/sceGuTexScale at all. This port keeps ioquake3's own
	 ortho projection and therefore stays on the 3D pipe everywhere, which is
	 what exposes the default.
	*/
	sceGuTexScale( 1.0f, 1.0f );
	sceGuTexOffset( 0.0f, 0.0f );

	sceGuFinish();
	sceGuSync( 0, 0 );

	sceDisplayWaitVblankStart();
	sceGuDisplay( GU_TRUE );

	/*
	 Leave a list open. Every qgl* body appends to the currently open list
	 and nothing else may call sceGuFinish - that is why qglFinish and
	 qglFlush are no-ops in psp_qgl.c. GLimp_EndFrame closes this list and
	 opens the next one.
	*/
	pspGuReady = qtrue;
	PSP_QGL_ResetStateCache();

	GLimp_BeginList();
}

#if PSP_GFX_DEBUG
/*
===============
GLimp_DebugProbe

A minimal test case that owes nothing to renderergl1 (q3 skill 14.5). Drawn
at the end of every frame, after the engine has had its turn.

  MAGENTA rectangle, top-left, GU_TRANSFORM_2D, untextured
      Bypasses the matrix stacks entirely - vertices are screen pixels. If
      this is visible, the display buffers, viewport, scissor, swap and
      sceGuDrawArray submission are all correct and the fault is upstream.

  CYAN rectangle, next to it, GU_TRANSFORM_3D through sceGumOrtho
      The same rectangle through the exact path the engine's 2D pass uses:
      an ortho projection, identity view and model, GU_VERTEX_32BITF. If the
      magenta one appears and this one does not, the projection path is the
      fault, not the draw path.

Both are untextured (GU_TCC_RGB via sceGuTexFunc is irrelevant with
GU_TEXTURE_2D disabled), so neither depends on psp_tex.c being correct.
State is restored afterwards; the engine re-issues its own matrices, cull
and texture state at the top of every frame regardless.
===============
*/
typedef struct {
	unsigned short	x, y, z;
} pspProbeVert2D_t;

typedef struct {
	float		x, y, z;
} pspProbeVert3D_t;

static void GLimp_DebugProbe( void )
{
	pspProbeVert2D_t	*v2;
	pspProbeVert3D_t	*v3;

	sceGuDisable( GU_TEXTURE_2D );
	sceGuDisable( GU_DEPTH_TEST );
	sceGuDisable( GU_BLEND );
	sceGuDisable( GU_CULL_FACE );

	// --- magenta, raw screen coordinates, no matrices involved ---
	v2 = (pspProbeVert2D_t *)sceGuGetMemory( 2 * sizeof( pspProbeVert2D_t ) );

	v2[ 0 ].x = 8;   v2[ 0 ].y = 8;   v2[ 0 ].z = 0;
	v2[ 1 ].x = 108; v2[ 1 ].y = 48;  v2[ 1 ].z = 0;

	sceGuColor( 0xFFFF00FF );
	sceGuDrawArray( GU_SPRITES, GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v2 );

	// --- cyan, through an ortho projection, the engine's 2D path ---
	sceGumMatrixMode( GU_PROJECTION );
	sceGumLoadIdentity();
	sceGumOrtho( 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, 0, 0, 1 );
	sceGumMatrixMode( GU_MODEL );
	sceGumLoadIdentity();
	sceGumUpdateMatrix();

	v3 = (pspProbeVert3D_t *)sceGuGetMemory( 6 * sizeof( pspProbeVert3D_t ) );

	v3[ 0 ].x = 120.0f; v3[ 0 ].y = 8.0f;  v3[ 0 ].z = 0.0f;
	v3[ 1 ].x = 220.0f; v3[ 1 ].y = 8.0f;  v3[ 1 ].z = 0.0f;
	v3[ 2 ].x = 220.0f; v3[ 2 ].y = 48.0f; v3[ 2 ].z = 0.0f;
	v3[ 3 ].x = 120.0f; v3[ 3 ].y = 8.0f;  v3[ 3 ].z = 0.0f;
	v3[ 4 ].x = 220.0f; v3[ 4 ].y = 48.0f; v3[ 4 ].z = 0.0f;
	v3[ 5 ].x = 120.0f; v3[ 5 ].y = 48.0f; v3[ 5 ].z = 0.0f;

	sceGuColor( 0xFFFFFF00 );
	sceGuDrawArray( GU_TRIANGLES, GU_VERTEX_32BITF | GU_TRANSFORM_3D, 6, 0, v3 );

	sceGuColor( 0xFFFFFFFF );
	sceGuEnable( GU_CULL_FACE );
	sceGuEnable( GU_DEPTH_TEST );
	sceGuEnable( GU_TEXTURE_2D );
}
#endif

/*
===============
GLimp_PrintMemReport

"pspgfxmem" console command. Texture RAM, VRAM still free, and the
display-list budget, in one place.
===============
*/
static void GLimp_PrintMemReport( void )
{
	PSP_TexMemReport();
	PSP_DrawMemReport();

	ri.Printf( PRINT_ALL, "PSP VRAM: %u bytes free after framebuffers\n",
		(unsigned int)vmemavail() );
}

/*
===============
GLimp_Init
===============
*/
void GLimp_Init( qboolean fixedFunction )
{
	ri.Printf( PRINT_ALL, "Initializing PSP GU renderer\n" );

	GLimp_ClaimVRAM();

	/*
	 Heap, not .bss, and 64-byte aligned so it cannot share a cache line
	 with anything else. sceGuStart ORs 0x40000000 onto this pointer
	 (verified by disassembling libpspgu's sceGuStart), so the GE reads the
	 command stream through the uncached mirror and no writeback is needed
	 for it. Vertex and index data no longer live here - psp_draw.c's arena
	 is cached and flushes itself per draw.
	*/
	pspDisplayList = (unsigned int *)memalign( 64, PSP_DISPLAY_LIST_BYTES );

	if( !pspDisplayList )
	{
		ri.Error( ERR_FATAL, "GLimp_Init: cannot allocate %d byte display list",
			PSP_DISPLAY_LIST_BYTES );
		return;
	}

	if( !PSP_DrawInitArena() )
	{
		ri.Error( ERR_FATAL, "GLimp_Init: cannot allocate the vertex arena" );
		return;
	}

	GLimp_InitGU();

	// The GE owns the panel now; pspDebugScreen must stop writing to it.
	CON_SetDisplayOwnedByGU( qtrue );

	/*
	 The panel is 480x272 and there is no mode list to pick from. Publish it
	 through the custom-mode cvars so the engine and the UI agree with the
	 hardware instead of with ioquake3's desktop mode table.
	*/
	ri.Cvar_Set( "r_mode", "-1" );
	ri.Cvar_Set( "r_customwidth", "480" );
	ri.Cvar_Set( "r_customheight", "272" );
	ri.Cvar_Set( "r_customaspect", "1" );
	ri.Cvar_Set( "r_fullscreen", "1" );

	glConfig.vidWidth     = PSP_SCREEN_WIDTH;
	glConfig.vidHeight    = PSP_SCREEN_HEIGHT;
	glConfig.windowAspect = (float)PSP_SCREEN_WIDTH / (float)PSP_SCREEN_HEIGHT;

	glConfig.colorBits   = 16;	// GU_PSM_5650
	glConfig.depthBits   = 16;
	glConfig.stencilBits = 0;	// no destination alpha in 5650

	glConfig.driverType   = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;

	glConfig.deviceSupportsGamma  = qfalse;
	glConfig.textureCompression   = TC_NONE;
	glConfig.textureEnvAddAvailable = qfalse;

	// One texture unit: tr_init.c takes the no-multitexture path
	// (the same one code/sdl/sdl_glimp.c:993 falls back to).
	glConfig.numTextureUnits = 1;
	glConfig.maxTextureSize  = 512;	// GE hardware limit, power of two

	glConfig.isFullscreen     = qtrue;
	glConfig.stereoEnabled    = qfalse;
	glConfig.displayFrequency = 60;	// 59.94 Hz, every model, every region

	Q_strncpyz( glConfig.vendor_string,   "Sony",  sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, "PSPGU", sizeof( glConfig.renderer_string ) );
	Q_strncpyz( glConfig.version_string,  "sceGu 1.1", sizeof( glConfig.version_string ) );
	glConfig.extensions_string[ 0 ] = '\0';

	ri.Cvar_Set( "r_lastValidRenderer", "PSPGU" );

	/*
	 Session 10 tunes r_picmip, com_soundMegs and the rest against measured
	 peaks, and this is where its texture number comes from. A console
	 command rather than a print at a fixed point in R_Init, because
	 renderergl1/ stays byte-identical to upstream and because the number
	 worth having is the one after a map has loaded, not after the menu.
	*/
	ri.Cmd_AddCommand( "pspgfxmem", GLimp_PrintMemReport );

	/*
	 r_pspClip. Registered here for the same reason as pspgfxmem -
	 renderergl1/ stays byte-identical to upstream, so a PSP-only cvar
	 cannot be declared in tr_init.c. See psp_draw.c for what it gates and
	 why it must be toggleable without a rebuild.
	*/
	r_pspClear = ri.Cvar_Get( "r_pspClear", "0", CVAR_ARCHIVE );

	/*
	 Wait for vblank every frame while a UDP socket is bound, even when the
	 vcount check says it is unnecessary for tearing. See the block in
	 GLimp_EndFrame for what it is actually buying.

	 Default 0 as of Session 12b, was 1. Session 11f added this as the
	 belt-and-braces half of the netcode fix - the real fix was moving
	 sceNetInit's callout and intr threads to 0x18, above the main thread, so
	 they preempt instead of waiting for the game loop to yield - and left the
	 A/B open in writing: "r_pspNetVblank 0 tests the priority fix alone; if it
	 holds, the default should flip to 0."

	 What forced the question: the socket is bound at boot whenever the WLAN
	 switch is on, which is the performance ledger's own declared workload
	 state. So this wait padded EVERY frame out to a whole 16.68 ms refresh and
	 frame time could only land on 3, 4, 5 or 6 of them - 20 / 15 / 12 / 10 fps
	 and nothing in between. That quantisation lands directly on the median and
	 p95 the ledger uses as its primary statistics, so no A/B was readable while
	 it was on.

	 Set to 1 in baseq3/autoexec.cfg to restore Session 9/11f behaviour without
	 a rebuild - which is also how the netplay half of this A/B gets run.

	 NOT archived: this is a diagnostic lever, and an archived value from a test
	 run silently deciding a later measurement is exactly how net_apctl wasted a
	 run in Session 9.
	*/
	r_pspNetVblank = ri.Cvar_Get( "r_pspNetVblank", "0", 0 );

	PSP_DrawRegisterCvars();

	ri.IN_Init( NULL );
}

/*
===============
GLimp_EndFrame

Closes the frame's display list, waits out the frame, flips, and opens the
next list. sceGuSwapBuffers exchanges the GE's draw and display targets, so
the local pointers have to follow it or GLimp_Shutdown frees the wrong two
and Session 6's readback paths sample the wrong buffer.
===============
*/
void GLimp_EndFrame( void )
{
	if( !pspGuReady )
		return;

#if PSP_GFX_DEBUG
	{
		static int	frames = 0;

		if( frames < 3 )
		{
			frames++;

			/*
			 The one fact the first diagnostic round could not establish:
			 whether Com_Frame reaches the renderer at all. No ORTHO, BIND
			 or DRAW line appeared, which is equally consistent with "the
			 engine issues no 2D commands" and "the engine never completes
			 a frame". This line separates them.
			*/
			ri.Printf( PRINT_ALL, "FRAME %d endframe\n", frames );
		}
	}

	GLimp_DebugProbe();
#endif

	/*
	 The two waits are timed separately (Session 11c). By this point the CPU
	 has submitted the whole list, so sceGuSync measures any residual GE work
	 before that list is complete, while sceDisplayWaitVblankStart is refresh
	 pacing and should be ~0 at anything below 59.94 fps. Keeping them apart
	 stops a vblank wait from being read as GPU cost. A zero sync wait means
	 only that GE completion did not extend this sampled frame; see
	 PSP_ZONE_GESYNC in psp_platform.h.
	*/
	Sys_PSP_ZoneBegin( PSP_ZONE_GESYNC );
	sceGuFinish();
	sceGuSync( 0, 0 );
	Sys_PSP_ZoneEnd( PSP_ZONE_GESYNC );

	Sys_PSP_ZoneBegin( PSP_ZONE_VBLANK );
	/*
	 Wait ONLY when this frame closed inside the same refresh as the previous
	 flip. Then - and only then - the queued flip has not happened yet, the
	 buffer we are about to rotate onto is still owned by the panel, and
	 drawing into it would tear.

	 Below 59.94 fps, which is every gameplay frame this port produces, the
	 vcount has already moved and the wait is skipped entirely. That is the
	 7-10 ms/frame Session 11c measured being spent on nothing.

	 sceDisplaySetFrameBuf replaces sceGuSwapBuffers because the GU's own
	 swap only knows about two buffers. NEXTFRAME keeps the flip aligned to
	 the refresh, so skipping the wait costs no tearing on the panel side.
	*/
	/*
	 SECOND reason to wait, and it has nothing to do with tearing (Session 12).

	 The PSP scheduler is strict-priority and does not rotate equal-priority
	 threads by itself. Before Session 11d this wait was unconditional, so the
	 main thread blocked 7-10 ms every frame and everything at its own priority
	 - including sceNetInit's callout and intr threads - ran in that window.
	 Making the wait conditional removed the gameplay loop's last block
	 (geSync 0 ms, NET_Sleep selects with a zero timeout, psp_input.c uses
	 Peek), and the network stack stopped being scheduled except by accident.

	 psp_net.c now creates those threads at 0x18, above the main thread, which
	 is the real fix: they preempt instead of waiting for a yield. This was the
	 belt-and-braces half - a guaranteed rendezvous once a frame - kept because
	 it is what Session 9 shipped working and what Quake3PSP-mirror ships
	 (unix/glimp.cpp:92-95 waits "only if we are in network mode", the identical
	 trade made the other way round).

	 Session 12b turned it OFF by default. It cost a whole refresh of every
	 frame whenever the WLAN switch was on, which quantised frame time to
	 multiples of 16.68 ms and made the profiler's median and p95 unreadable.
	 The clause is still here, and r_pspNetVblank 1 restores it, because the
	 priority-fix-alone A/B this enables is the one Session 11f wrote down and
	 never ran.

	 The cvar remains pinned to 1 for network compatibility, but the effective
	 wait is now gated on a remote client session as well as the socket. WLAN
	 can leave the socket bound in menus, local play and demo playback; those
	 paths no longer pay the network rendezvous. Remote LAN/internet sessions
	 still do.
	*/
	if( sceDisplayGetVcount() == pspLastFlipVcount ||
		( r_pspNetVblank && r_pspNetVblank->integer &&
		  NET_PSP_IsSocketOpen() && CL_PSP_IsRemoteSession() ) )
		sceDisplayWaitVblankStart();

	sceDisplaySetFrameBuf( (void *)pspFrameBuffer[ pspDrawIndex ],
		PSP_BUFFER_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_565,
		PSP_DISPLAY_SETBUF_NEXTFRAME );

	pspLastFlipVcount = sceDisplayGetVcount();
	pspDrawIndex      = ( pspDrawIndex + 1 ) % PSP_NUM_FRAMEBUFFERS;
	Sys_PSP_ZoneEnd( PSP_ZONE_VBLANK );

	GLimp_BeginList();
}
