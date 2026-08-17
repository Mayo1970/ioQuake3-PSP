/*
===========================================================================
PSP port - code/psp/psp_draw.c

Session 6: GL vertex arrays -> one interleaved GE vertex array per draw,
appended to the display list psp_glimp.c keeps open.
Session 7: immediate mode, and the frustum clipper the GE needs.

Why interleaving is unavoidable: GL lets every attribute live in its own
array with its own stride, and ioquake3 uses that - tess.xyz is stride 16
(padded for SIMD), tess.svars.texcoords and tess.svars.colors are tightly
packed in separate arrays. The GE reads ONE array with ONE fixed layout
described by the vtype word of sceGuDrawArray. So each draw copies the
attributes it was pointed at into a GE-shaped array carved out of the
display list.

Vertex layout is Quake3PSP-mirror's glvert_t (renderer/tr_local.h:577,
used at tr_shade.cpp:189): texture coordinates, then colour, then
position. That order is not a preference - the GE requires attributes in
exactly the order the vtype bits are defined, and position last.
===========================================================================
*/

#include "psp_draw.h"
#include "psp_static_world.h"
#include "psp_gu.h"

#include <pspgum.h>
#include <pspkernel.h>	// sceKernelDcacheWritebackRange - the arena is cached
#include <malloc.h>	// memalign - the arena must be cache-line aligned

/*
 One layout for every draw: GU_TEXTURE_32BITF | GU_COLOR_8888 |
 GU_VERTEX_32BITF, 24 bytes.

 Emitting texture coordinates even when no texcoord array is bound costs 8
 bytes on the handful of untextured draws and buys a single code path
 instead of one per combination of enabled arrays.
*/
typedef struct {
	float		u, v;
	unsigned int	rgba;		// 0xAABBGGRR, the byte order GL_UNSIGNED_BYTE RGBA already has
	float		x, y, z;
} pspVert_t;

#define PSP_VTYPE_BASE	( GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D )

/*
 sceGuGetMemory carves out of the display list and - read the source,
 pspsdk-src/src/gu/sceGuGetMemory.c - performs NO bounds check and never
 returns NULL. Overrunning the list writes past the allocation psp_glimp.c
 made and corrupts the heap behind it.

 Session 7 handled that with a byte budget policed here, since the list held
 vertex data as well as commands. Session 10 moved the data out (see the
 arena note below), so nothing in this file calls sceGuGetMemory any more
 and the budget is gone with it. The display list now carries commands only
 and psp_glimp.c sizes it accordingly.
*/

/*
===========================================================================
Session 10: the vertex arena.

Everything above was written when vertex and index data came out of the
display list via sceGuGetMemory. That put every vertex byte behind the
uncached mirror, because sceGuStart ORs 0x40000000 onto the list pointer -
which is convenient (no writeback to get wrong) and slow, since bulk
uncached stores neither coalesce in the write buffer nor benefit from the
cache. The clipper made it worse: PSP_VertexOutcode and PSP_ClipTriangle
READ the vertices back, so clipped geometry paid uncached latency twice.

Session 10's frame breakdown measured re.EndFrame at ~45% of a 124 ms frame
(~55 ms), with the list budget exhausted often enough to print
"display list full, N draws skipped" with N up to 42 - i.e. geometry was
being dropped on screen. Both symptoms point at the same place, and
psp_draw.c's own Session 7 note already named the fix:

  "If the skipped count is still non-zero on hardware, the next step is a
   separate vertex ring buffer with explicit sceKernelDcacheWritebackRange
   - NOT a bigger list."

So data now lives in a cached arena of its own and the display list carries
commands only.

Lifetime and safety
-------------------
This is a linear bump allocator reset once per frame, NOT a ring. That is
safe only because of the exact order GLimp_EndFrame runs in:

    sceGuFinish(); sceGuSync(0,0);   <- GE has drained the whole list
    ...swap...
    GLimp_BeginList() -> PSP_DrawBeginFrame() -> arena reset

sceGuSync guarantees the GE is finished with every draw that referenced
last frame's arena before a single byte of it is reused. If a future change
ever submits work that outlives sceGuSync, this becomes a real ring with
fencing and this comment becomes a bug.

Coherency
---------
Each draw writes its data back with sceKernelDcacheWritebackRange
immediately before its sceGuDrawArray. Per-draw rather than once per frame
because GU_DIRECT means the GE is already executing the list as it is
written - a single flush at frame end would be far too late.

Writeback, never WritebackInvalidate: the 64-byte line granularity means a
range can share its first and last line with a neighbouring allocation, and
writing back a neighbour's dirty bytes is harmless where invalidating them
would not be. Allocations are 64-byte aligned anyway, which makes sharing
rare, but the choice of primitive is what makes it safe rather than lucky.
===========================================================================
*/
/*
 4 MB, was 2 (Session 11d).

 2 MB was never measured as sufficient - it was measured as "never
 overflowed" on a Session 10 build that ran at 8 fps in simple views. Session
 11d hit it properly: q3dm1 in a firefight dropped 17, then 122, then 193
 draws in single frames, which is most of the world disappearing while the
 player is looking at it.

 The demand is real rather than wasteful. Every shader stage re-enters
 PSP_DrawElements with the same tess vertices and different texcoords or
 colours, and each pass repacks all of them at 24 bytes. A multi-stage view
 of q3dm1 therefore wants several MB per frame. Cutting that means cutting
 PASSES, which is renderer work; until then the arena has to cover it.

 The 2 MB comes from what Session 11d freed: textures moved to the volatile
 partition and VRAM, which took ~4.2 MB out of the newlib heap (measured:
 heap arena 35065 -> 30879 KB). This is allocated at GLimp_Init, before any
 texture exists, so it gets contiguous space rather than competing with the
 fragmentation that made 128 KB uploads fail.
*/
#define PSP_ARENA_BYTES		( 4 * 1024 * 1024 )
#define PSP_ARENA_MIN_BYTES	( 1 * 1024 * 1024 )
#define PSP_ARENA_ALIGN		64

static byte		*pspArena;
static int		pspArenaBytes;		// what was actually obtained
static int		pspArenaUsed;		// bytes taken this frame
static int		pspArenaPeak;		// high-water since the last report

typedef struct {
	pspVert_t			*vertices;
	unsigned short		*indices;
	int					vertexCount;
	int					indexCount;
} pspStaticWorldRun_t;

static pspStaticWorldRun_t	pspStaticWorldRun;

#define PSP_STATIC_WORLD_RUN_INDEX_CAP	8192
static unsigned short	pspStaticWorldRunIndices[ PSP_STATIC_WORLD_RUN_INDEX_CAP ];

/*
===============
PSP_ArenaTake

Bump-allocates cache-line-aligned space for one draw's data. Returns NULL
when the frame's arena is exhausted; every caller must treat that exactly
as the old budget check did - count a skipped draw and bail - rather than
drawing from a null pointer.
===============
*/
static void *PSP_ArenaTake( int bytes )
{
	byte	*p;

	if( !pspArena || bytes <= 0 )
		return NULL;

	bytes = ( bytes + ( PSP_ARENA_ALIGN - 1 ) ) & ~( PSP_ARENA_ALIGN - 1 );

	if( pspArenaUsed + bytes > pspArenaBytes )
		return NULL;

	p             = pspArena + pspArenaUsed;
	pspArenaUsed += bytes;

	if( pspArenaUsed > pspArenaPeak )
		pspArenaPeak = pspArenaUsed;

	return p;
}

/*
===============
PSP_ArenaTakeUnpadded

Bump-allocates exactly the requested number of bytes. Static-world vertices
use this after the first surface so that the run has no per-surface padding.
===============
*/
static void *PSP_ArenaTakeUnpadded( int bytes )
{
	byte	*p;

	if( !pspArena || bytes <= 0 )
		return NULL;

	if( pspArenaUsed + bytes > pspArenaBytes )
		return NULL;

	p             = pspArena + pspArenaUsed;
	pspArenaUsed += bytes;

	if( pspArenaUsed > pspArenaPeak )
		pspArenaPeak = pspArenaUsed;

	return p;
}

/*
===============
PSP_ArenaFlush

Makes one draw's data visible to the GE. See the coherency note above for
why this is per-draw and why it is Writeback and not WritebackInvalidate.
===============
*/
static ID_INLINE void PSP_ArenaFlush( const void *p, int bytes )
{
	if( p && bytes > 0 )
		sceKernelDcacheWritebackRange( p, (unsigned int)bytes );
}

/*
===============
PSP_DrawStaticWorldAppend

The static-world cache owns the source blocks. This seam copies one cached
surface into the ordinary per-frame arena and rebases its indices against the
run's first vertex. Indices stay in the run scratch array until submit, so the
vertices remain one contiguous arena allocation. Both capacity checks happen
before the arena is changed, so a full arena or scratch array cleanly falls
back to the existing renderer path.
===============
*/
qboolean PSP_DrawStaticWorldAppend( const void *vertices, int vertexCount,
	const unsigned short *indices, int indexCount )
{
	pspVert_t			*dstVertices;
	int					vertexBytes;
	int					vertexAlloc;
	qboolean				firstSurface;
	int					i;

	if( indexCount == 0 )
		return qtrue;

	if( !pspGuReady || !pspArena || !vertices || vertexCount <= 0 ||
		!indices || indexCount < 0 )
		return qfalse;

	if( pspStaticWorldRun.vertexCount > 65536 - vertexCount )
		return qfalse;

	if( pspStaticWorldRun.indexCount > PSP_STATIC_WORLD_RUN_INDEX_CAP - indexCount )
		return qfalse;

	firstSurface = ( pspStaticWorldRun.vertexCount == 0 );
	vertexBytes = vertexCount * (int)sizeof( pspVert_t );
	vertexAlloc = firstSurface ?
		( vertexBytes + ( PSP_ARENA_ALIGN - 1 ) ) &
			~( PSP_ARENA_ALIGN - 1 ) : vertexBytes;

	if( vertexAlloc > pspArenaBytes - pspArenaUsed )
		return qfalse;

	dstVertices = (pspVert_t *)( firstSurface ?
		PSP_ArenaTake( vertexBytes ) : PSP_ArenaTakeUnpadded( vertexBytes ) );
	if( !dstVertices )
		return qfalse;

	if( firstSurface )
	{
		/* Reclaim the first allocation's tail so the next take starts at the
		 * true end of this surface's vertices. */
		pspArenaUsed -= vertexAlloc - vertexBytes;
	}
	else if( dstVertices != pspStaticWorldRun.vertices +
		pspStaticWorldRun.vertexCount )
	{
		return qfalse;
	}

	Com_Memcpy( dstVertices, vertices, vertexBytes );
	for( i = 0; i < indexCount; i++ )
		pspStaticWorldRunIndices[ pspStaticWorldRun.indexCount + i ] =
			(unsigned short)( indices[ i ] + pspStaticWorldRun.vertexCount );

	if( firstSurface )
		pspStaticWorldRun.vertices = dstVertices;

	pspStaticWorldRun.vertexCount += vertexCount;
	pspStaticWorldRun.indexCount += indexCount;
	return qtrue;
}

/*
===============
PSP_DrawStaticWorldSubmit

The run remains available until PSP_DrawStaticWorldEnd so the renderer can
submit the same coalesced geometry for each shader stage without copying it
again. The scratch indices are copied into one arena block on the first
submit; later submits reuse that block. The cached arena data is ordinary
cached memory, so it follows the same per-draw writeback discipline as
PSP_DrawElements.
===============
*/
qboolean PSP_DrawStaticWorldSubmit( void )
{
	int				indexBytes;
	unsigned short	*indexBlock;

	if( !pspStaticWorldRun.vertices ||
		pspStaticWorldRun.vertexCount <= 0 || pspStaticWorldRun.indexCount <= 0 )
		return qfalse;

	if( !pspStaticWorldRun.indices )
	{
		indexBytes = pspStaticWorldRun.indexCount * (int)sizeof( unsigned short );
		indexBlock = (unsigned short *)PSP_ArenaTake( indexBytes );
		if( !indexBlock )
			return qfalse;

		Com_Memcpy( indexBlock, pspStaticWorldRunIndices, indexBytes );
		pspStaticWorldRun.indices = indexBlock;
	}

	PSP_ArenaFlush( pspStaticWorldRun.vertices,
		pspStaticWorldRun.vertexCount * (int)sizeof( pspVert_t ) );
	PSP_ArenaFlush( pspStaticWorldRun.indices,
		pspStaticWorldRun.indexCount * (int)sizeof( unsigned short ) );

	sceGuDrawArray( GU_TRIANGLES, PSP_VTYPE_BASE | GU_INDEX_16BIT,
		pspStaticWorldRun.indexCount, pspStaticWorldRun.indices,
		pspStaticWorldRun.vertices );
	return qtrue;
}

void PSP_DrawStaticWorldEnd( void )
{
	if( pspStaticWorldRun.vertices )
	{
		const int alignedUsed = ( pspArenaUsed + ( PSP_ARENA_ALIGN - 1 ) ) &
			~( PSP_ARENA_ALIGN - 1 );

		if( alignedUsed <= pspArenaBytes )
		{
			pspArenaUsed = alignedUsed;
			if( pspArenaUsed > pspArenaPeak )
				pspArenaPeak = pspArenaUsed;
		}
	}

	pspStaticWorldRun.vertices = NULL;
	pspStaticWorldRun.indices = NULL;
	pspStaticWorldRun.vertexCount = 0;
	pspStaticWorldRun.indexCount = 0;
}

/*
===============
PSP_DrawInitArena / PSP_DrawShutdownArena

Called from GLimp_Init / GLimp_Shutdown, beside the display list they
replace half of.
===============
*/
qboolean PSP_DrawInitArena( void )
{
	int	want;

	if( pspArena )
		return qtrue;

	/*
	 Halve on failure rather than giving up. A smaller arena drops draws under
	 load, which is bad; no arena at all draws nothing, which is worse - and
	 the size that was obtained is printed, so a degraded run says so instead
	 of looking like a renderer bug.
	*/
	for( want = PSP_ARENA_BYTES; want >= PSP_ARENA_MIN_BYTES; want /= 2 )
	{
		pspArena = (byte *)memalign( PSP_ARENA_ALIGN, want );

		if( pspArena )
			break;
	}

	if( !pspArena )
		return qfalse;

	pspArenaBytes = want;
	pspArenaUsed  = 0;
	pspArenaPeak  = 0;

	ri.Printf( PRINT_ALL, "PSP vertex arena: %d KB%s\n", pspArenaBytes / 1024,
		( pspArenaBytes < PSP_ARENA_BYTES ) ? " (reduced - heap was short)" : "" );

	return qtrue;
}

/*
===============
PSP_DrawArenaPeakKB

For the periodic frame report. The Session 10 run proved the arena never
overflowed but never said by how much, so it could not be sized down to
return heap to the PRX budget. This is that number.
===============
*/
int PSP_DrawArenaPeakKB( void )
{
	const int	peak = pspArenaPeak;

	/*
	 Reset on read, so this is the peak SINCE THE LAST REPORT rather than
	 since boot. A lifetime high-water pinned itself at the cap the first time
	 any frame saturated and then read 2048 forever - which is exactly what it
	 did across Sessions 11b-d, hiding both how close the normal case was and
	 whether a change had helped.
	*/
	pspArenaPeak = 0;

	return peak / 1024;
}

void PSP_DrawShutdownArena( void )
{
	if( pspArena )
	{
		free( pspArena );
		pspArena = NULL;
	}

	pspArenaBytes = 0;
	pspArenaUsed  = 0;
	pspArenaPeak  = 0;
}

typedef struct {
	const byte	*ptr;
	int		stride;
	qboolean	enabled;
} pspArray_t;

static pspArray_t	pspVertexArray;
static pspArray_t	pspTexCoordArray;
static pspArray_t	pspColorArray;

static unsigned int	pspCurrentColor = 0xFFFFFFFF;

static int		pspSkippedDraws;	// this frame
static int		pspSkippedTotal;
static int		pspClippedTris;		// this frame, for pspgfxmem
static int		pspClippedPrev;		// last complete frame


/*
===============
PSP_DrawBeginFrame

Called from GLimp_BeginList, immediately after sceGuStart resets the list.
===============
*/
void PSP_DrawBeginFrame( void )
{
	PSP_DrawInvalidateBatchCache();

	if( pspSkippedDraws )
	{
		/*
		 A draw dropped for a full display list looks exactly like "the
		 port is just slow" if it is not said out loud. Report the frame,
		 not every draw.
		*/
		ri.Printf( PRINT_WARNING, "PSP_DrawElements: vertex arena full (%d KB), "
			"%d draws skipped\n",
			pspArenaBytes / 1024, pspSkippedDraws );

		pspSkippedTotal += pspSkippedDraws;
		pspSkippedDraws  = 0;
	}

	pspClippedPrev = pspClippedTris;	// pspgfxmem runs mid-frame; report a whole one
	pspClippedTris = 0;

	/*
	 Safe only because GLimp_EndFrame did sceGuFinish + sceGuSync before
	 calling GLimp_BeginList, so the GE is provably done with everything
	 last frame's arena handed it. See the arena note above.
	*/
	pspArenaUsed = 0;
}

/*
=================================================================
Client state

Upstream only ever uses three shapes, all of them here:
  qglVertexPointer  ( 3, GL_FLOAT,         16, ... )   tess.xyz, padded
  qglTexCoordPointer( 2, GL_FLOAT,       0/16, ... )   svars.texcoords / texCoords
  qglColorPointer   ( 4, GL_UNSIGNED_BYTE,  0, ... )   svars.colors
A stride of 0 means tightly packed, which is GL's rule, not a shortcut.
=================================================================
*/

void PSP_DrawEnableClientState( GLenum cap, qboolean enable )
{
	switch( cap )
	{
		case GL_VERTEX_ARRAY:		pspVertexArray.enabled   = enable; break;
		case GL_TEXTURE_COORD_ARRAY:	pspTexCoordArray.enabled = enable; break;
		case GL_COLOR_ARRAY:		pspColorArray.enabled    = enable; break;
		default:			break;
	}
}

void PSP_DrawVertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr )
{
	pspVertexArray.ptr    = (const byte *)ptr;
	pspVertexArray.stride = stride ? stride : (int)( size * sizeof( float ) );
}

void PSP_DrawTexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr )
{
	pspTexCoordArray.ptr    = (const byte *)ptr;
	pspTexCoordArray.stride = stride ? stride : (int)( size * sizeof( float ) );
}

void PSP_DrawColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr )
{
	pspColorArray.ptr    = (const byte *)ptr;
	pspColorArray.stride = stride ? stride : size;
}

/*
 The colour used for every vertex when GL_COLOR_ARRAY is off. GL's
 RGBA byte order and the GE's 0xAABBGGRR are the same bytes in the same
 places on a little-endian machine, so packing is a shift, not a swap.
*/
void PSP_DrawSetColor4f( GLfloat r, GLfloat g, GLfloat b, GLfloat a )
{
	pspCurrentColor =
		( (unsigned int)( Com_Clamp( 0.0f, 1.0f, r ) * 255.0f )       ) |
		( (unsigned int)( Com_Clamp( 0.0f, 1.0f, g ) * 255.0f ) <<  8 ) |
		( (unsigned int)( Com_Clamp( 0.0f, 1.0f, b ) * 255.0f ) << 16 ) |
		( (unsigned int)( Com_Clamp( 0.0f, 1.0f, a ) * 255.0f ) << 24 );
}

void PSP_DrawSetColor4ubv( const GLubyte *v )
{
	if( !v )
		return;

	pspCurrentColor = (unsigned int)v[0] | ( (unsigned int)v[1] << 8 ) |
	                  ( (unsigned int)v[2] << 16 ) | ( (unsigned int)v[3] << 24 );
}

/*
===========================================================================
CLIPPING

Ported from Quake3PSP-mirror renderer/clipping.cpp (Peter Mackay and Chris
Swindle, 2007; Crow_bar 2009-2010) - the only Quake 3 clipper proven on PSP
hardware.

Q3PORT.md's Session 7 checklist said to integrate DaedalusX64's
VectorClipping.S / _ClipToHyperPlane instead. Reading both first, as the
project rule requires, that is wrong twice over:

  1. _ClipToHyperPlane clips in CLIP SPACE, after transform, because
     Daedalus does its own software T&L (TnLVFPU.S) and hands the GE
     screen-space vertices. This port uses the GE's hardware T&L with
     GU_TRANSFORM_3D and sceGum matrices, so there are no post-transform
     vertices to give it. It does not apply at all.

  2. The plane that needs clipping here is not the near plane. The mirror
     ships (clipping.cpp:23-28) with

         CLIP_LEFT 1  CLIP_RIGHT 1  CLIP_BOTTOM 1  CLIP_TOP 1
         CLIP_NEAR 0  CLIP_FAR  0

     The GE's screen-space coordinate range is finite; triangles reaching
     far outside the viewport wrap rather than clip. The four SIDE planes
     are the ones that matter, and near/far are deliberately off.

Where this differs from the mirror, and why:

  - The mirror clips inside its renderer, per triangle, recomputing four
    dot products for all three vertices of every triangle - O(3T) VFPU
    blocks, and a large part of its documented "low framerate". This port
    keeps renderergl1/ byte-identical, so the clipper sits behind
    qglDrawElements where the WHOLE BATCH is visible: one outcode per
    VERTEX (O(V)), then three cheap ORs per triangle. Wholly-inside
    triangles keep their indices and stay in the fast indexed draw.

  - The mirror's calculate_intersection has two defects that are not
    reproduced here: ColorClampf/ColorClampi take their arguments by value
    and therefore clamp nothing, and the colour delta r[2]..a[2] is
    consumed after being overwritten. Colour is interpolated per channel
    and clamped properly below.

  - Clip planes are re-loaded into the VFPU at the top of each draw rather
    than held across the frame. The mirror leaves them in C700-C730 from
    begin_frame to end_frame and assumes nothing else touches block 7;
    reloading is four instructions and removes the question entirely.
===========================================================================
*/

#define PSP_CLIP_PLANES		4

// Sutherland-Hodgman on a triangle against 4 planes yields at most 3+4 = 7
// vertices, i.e. 5 triangles = 15 vertices emitted.
#define PSP_CLIP_WORK_VERTS	16
#define PSP_CLIP_MAX_OUT	15

// Outcode scratch. SHADER_MAX_VERTEXES is 1000; a batch wider than this
// skips clipping rather than overrunning.
#define PSP_CLIP_MAX_VERTS	2048

/*
 A tessellated surface can invoke qglDrawElements once per shader pass. The
 position and index arrays are invariant across those passes, while UVs and
 colours are deliberately not. Keep only the invariant work here, and keep
 the cache valid until RB_BeginSurface starts the next tessellator batch.

 The packed index buffer lives in the frame arena, not in static storage: the
 GE may still be reading a previous surface when the next surface begins, so
 replacing a static buffer would race the display list. The outcodes are
 private CPU data and can live in the cache itself.
*/
typedef struct
{
	const byte			*vertexPtr;
	int					vertexStride;
	const unsigned int	*indices;
	int					count;
	qboolean			clipRequested;
	qboolean			clip;
	qboolean			valid;
	int					vertCount;
	int					insideIndices;
	int					clipTris;
	unsigned short		*packedIndices;
	byte					outcode[ PSP_CLIP_MAX_VERTS ];
} pspBatchCache_t;

static pspBatchCache_t					pspBatchCache;

void PSP_DrawInvalidateBatchCache( void )
{
	pspBatchCache.valid = qfalse;
}

static ScePspFVector4 __attribute__((aligned(16)))	pspClipPlane[ PSP_CLIP_PLANES ];
static qboolean						pspClipPlanesValid;
static qboolean						pspMatrixDirty = qtrue;
static qboolean						pspOrtho2D;

static cvar_t						*r_pspClip;
static cvar_t						*r_pspFastTexCoords;

/*
 psp_qgl.c's view of GL_MATRIX_MODE, so the plane extraction can borrow the
 gum matrix stacks and put the mode back exactly as it found it.
*/
extern int	pspMatrixModeCurrent;

/*
===============
PSP_DrawRegisterCvars

Called from GLimp_Init. r_pspClip is registered here rather than in
renderergl1/, which stays byte-identical to upstream.

It exists to be turned OFF on hardware. Session 6 lost two rounds to a
symptom that belonged to something else entirely; "\r_pspClip 0" makes
"the geometry is wrong because the clipper is wrong" and "the geometry is
wrong for some other reason" one console command apart instead of one
rebuild apart.
===============
*/
void PSP_DrawRegisterCvars( void )
{
	/*
	 Deliberately NOT CVAR_LATCH. The whole point is that it can be toggled
	 from the console mid-session, in front of the geometry being judged.
	*/
	r_pspClip = ri.Cvar_Get( "r_pspClip", "1", CVAR_ARCHIVE );

	/*
	 Session 12b. Whether psp_tcmod.c may point the texcoord array straight at
	 tess.texCoords for stages that need no per-vertex work, instead of letting
	 ComputeTexCoords copy 8 bytes per vertex into svars first. 1 = on, and
	 CONFIRMED CORRECT on hardware 2026-08-12 (PSP-2000, ARK-4). SELECT+L
	 toggles it.

	 It was briefly a four-level cvar, because this file also drove a GE
	 texture-matrix path that folded affine tcMod chains. Three hardware rounds
	 established that path renders wrongly and it has been removed; see
	 psp_tcmod.c and Q3PORT.md Session 12b before considering it again.

	 NOT archived, unlike r_pspClip: this is a measurement lever, and an
	 archived value from a test run silently deciding a later measurement is a
	 trap this port has already walked into once.
	*/
	r_pspFastTexCoords = ri.Cvar_Get( "r_pspFastTexCoords", "1", 0 );
	PSP_StaticWorld_RegisterCvar();
}

/*
===============
PSP_DrawSetOrtho

qglOrtho means RB_SetGL2D: a 2D pass, which must not be clipped. The mirror
gates on tess.transType != GU_TRANSFORM_2D for the same reason; this port
draws 2D through the 3D T&L pipe (see Q3PORT.md Session 6), so the flag has
to come from the projection instead of from the vertex type.
===============
*/
void PSP_DrawSetOrtho( qboolean ortho )
{
	pspOrtho2D     = ortho;
	pspMatrixDirty = qtrue;
	PSP_DrawInvalidateBatchCache();
}

/*
===============
PSP_DrawMatrixDirty

Any qgl* call that changes GU_MODEL or GU_PROJECTION invalidates the cached
planes. Recomputing them per draw would mean two sceGumStoreMatrix calls, a
4x4 multiply and four square roots for every surface in the map.
===============
*/
void PSP_DrawMatrixDirty( void )
{
	pspMatrixDirty = qtrue;
	PSP_DrawInvalidateBatchCache();
}

/*
===============
PSP_DrawFastTexCoords

r_pspFastTexCoords, read by psp_tcmod.c. Registered beside r_pspClip so every
PSP-only renderer cvar lives in one place.
===============
*/
qboolean PSP_DrawFastTexCoords( void )
{
	return ( r_pspFastTexCoords && r_pspFastTexCoords->integer ) ? qtrue : qfalse;
}

static void PSP_NormalisePlane( ScePspFVector4 *p )
{
	// sqrtf, never sqrt: the Allegrex FPU is single-precision only and the
	// double version is a software emulation call.
	float	len = sqrtf( p->x * p->x + p->y * p->y + p->z * p->z );

	if( len < 0.000001f )
		return;

	len = 1.0f / len;

	p->x *= len;
	p->y *= len;
	p->z *= len;
	p->w *= len;
}

/*
===============
PSP_UpdateClipPlanes

Extracts the four side planes of the frustum from the combined matrix, in
MODEL space - which is the space the vertices about to be submitted are in,
because the GE has not transformed them yet.

Gribb/Hartmann sum-and-difference of rows, exactly as clipping.cpp:111-157.
ScePspFMatrix4's .x/.y/.z/.w members are the four COLUMNS of a GL-layout
matrix, so comb.x.w is m[3], comb.y.x is m[4], and the expressions below are
the standard OpenGL-column-major form of the extraction.

GU_VIEW is pinned to identity for the life of the process (psp_glimp.c), so
the combined matrix is projection * model with nothing in between.
===============
*/
static void PSP_UpdateClipPlanes( void )
{
	ScePspFMatrix4 __attribute__((aligned(16)))	proj, model, comb;
	int						savedMode;

	if( !pspMatrixDirty )
		return;

	pspMatrixDirty     = qfalse;
	pspClipPlanesValid = qfalse;

	if( pspOrtho2D )
		return;

	savedMode = pspMatrixModeCurrent;

	sceGumMatrixMode( GU_PROJECTION );
	sceGumStoreMatrix( &proj );
	sceGumMatrixMode( GU_MODEL );
	sceGumStoreMatrix( &model );
	sceGumMatrixMode( savedMode );

	gumMultMatrix( &comb, &proj, &model );

	// LEFT   = row4 + row1
	pspClipPlane[ 0 ].x = comb.x.w + comb.x.x;
	pspClipPlane[ 0 ].y = comb.y.w + comb.y.x;
	pspClipPlane[ 0 ].z = comb.z.w + comb.z.x;
	pspClipPlane[ 0 ].w = comb.w.w + comb.w.x;

	// RIGHT  = row4 - row1
	pspClipPlane[ 1 ].x = comb.x.w - comb.x.x;
	pspClipPlane[ 1 ].y = comb.y.w - comb.y.x;
	pspClipPlane[ 1 ].z = comb.z.w - comb.z.x;
	pspClipPlane[ 1 ].w = comb.w.w - comb.w.x;

	// BOTTOM = row4 + row2
	pspClipPlane[ 2 ].x = comb.x.w + comb.x.y;
	pspClipPlane[ 2 ].y = comb.y.w + comb.y.y;
	pspClipPlane[ 2 ].z = comb.z.w + comb.z.y;
	pspClipPlane[ 2 ].w = comb.w.w + comb.w.y;

	// TOP    = row4 - row2
	pspClipPlane[ 3 ].x = comb.x.w - comb.x.y;
	pspClipPlane[ 3 ].y = comb.y.w - comb.y.y;
	pspClipPlane[ 3 ].z = comb.z.w - comb.z.y;
	pspClipPlane[ 3 ].w = comb.w.w - comb.w.y;

	PSP_NormalisePlane( &pspClipPlane[ 0 ] );
	PSP_NormalisePlane( &pspClipPlane[ 1 ] );
	PSP_NormalisePlane( &pspClipPlane[ 2 ] );
	PSP_NormalisePlane( &pspClipPlane[ 3 ] );

	pspClipPlanesValid = qtrue;
}

/*
===============
PSP_ClipActive

The clipper runs only for 3D draws, only when the planes came out of a real
matrix, and only when r_pspClip says so.
===============
*/
static qboolean PSP_ClipActive( void )
{
	if( r_pspClip && !r_pspClip->integer )
		return qfalse;

	PSP_UpdateClipPlanes();

	return ( !pspOrtho2D && pspClipPlanesValid ) ? qtrue : qfalse;
}

/*
===============
PSP_LoadClipPlanesVFPU

C700..C730 hold the four planes for the outcode loop below.
===============
*/
static void PSP_LoadClipPlanesVFPU( void )
{
	__asm__ volatile (
		"ulv.q	C700, %0\n"
		"ulv.q	C710, %1\n"
		"ulv.q	C720, %2\n"
		"ulv.q	C730, %3\n"
		:: "m"( pspClipPlane[ 0 ] ), "m"( pspClipPlane[ 1 ] ),
		   "m"( pspClipPlane[ 2 ] ), "m"( pspClipPlane[ 3 ] )
	);
}

/*
===============
PSP_VertexOutcode

One bit per plane, set when the vertex is on the OUTSIDE (negative) side.
Zero means wholly inside all four.

Four vdot.q against the plane block, exactly the mirror's inner loop
(clipping.cpp:251-263), with one change: the results come back as raw bit
patterns and only the sign bit is tested. The mirror declares them `float`
and constrains them to GPRs, which makes the compiler shuttle each one
back into an FP register just to compare it against zero.

ulv.q reads 16 bytes from &v->x, i.e. four bytes past the end of a 24-byte
pspVert_t. That fourth lane is immediately overwritten by vone.s, so its
value does not matter - but the READ still has to be in bounds. Every
caller allocates one spare vertex past the end for exactly this reason.
===============
*/
static ID_INLINE int PSP_VertexOutcode( const pspVert_t *v )
{
	unsigned int	d0, d1, d2, d3;

	__asm__ volatile (
		"ulv.q	C610, %4\n"		// C610 = { x, y, z, junk }
		"vone.s	S613\n"			// make it { x, y, z, 1 }
		"vdot.q	S620, C700, C610\n"
		"vdot.q	S621, C710, C610\n"
		"vdot.q	S622, C720, C610\n"
		"vdot.q	S623, C730, C610\n"
		"mfv	%0, S620\n"
		"mfv	%1, S621\n"
		"mfv	%2, S622\n"
		"mfv	%3, S623\n"
		: "=r"( d0 ), "=r"( d1 ), "=r"( d2 ), "=r"( d3 )
		: "m"( v->x )
	);

	return (int)( ( ( d0 >> 31 )       ) |
	              ( ( d1 >> 31 ) << 1 ) |
	              ( ( d2 >> 31 ) << 2 ) |
	              ( ( d3 >> 31 ) << 3 ) );
}

static ID_INLINE float PSP_PlaneDistance( const ScePspFVector4 *p, const pspVert_t *v )
{
	return p->x * v->x + p->y * v->y + p->z * v->z + p->w;
}

#ifndef PSP_CLIP_INTERSECT_VFPU
#define PSP_CLIP_INTERSECT_VFPU 0
#endif

/*
===============
PSP_ClipIntersect

The new vertex where edge v1->v2 crosses the plane. Position, texture
coordinates and all four colour channels are interpolated at the same
parameter.
===============
*/
#if !PSP_CLIP_INTERSECT_VFPU
static void PSP_ClipIntersectScalar( const ScePspFVector4 *plane, const pspVert_t *v1,
                                     const pspVert_t *v2, pspVert_t *out )
{
	float	dx = v2->x - v1->x;
	float	dy = v2->y - v1->y;
	float	dz = v2->z - v1->z;
	float	top, bottom, t;
	int	i;

	top    = PSP_PlaneDistance( plane, v1 );
	bottom = plane->x * dx + plane->y * dy + plane->z * dz;

	t = ( bottom != 0.0f ) ? ( -top / bottom ) : 0.0f;

	if( t < 0.0f )
		t = 0.0f;
	if( t > 1.0f )
		t = 1.0f;

	out->x = v1->x + t * dx;
	out->y = v1->y + t * dy;
	out->z = v1->z + t * dz;

	out->u = v1->u + t * ( v2->u - v1->u );
	out->v = v1->v + t * ( v2->v - v1->v );

	out->rgba = 0;

	for( i = 0; i < 4; i++ )
	{
		int	c1 = (int)( ( v1->rgba >> ( i * 8 ) ) & 0xFF );
		int	c2 = (int)( ( v2->rgba >> ( i * 8 ) ) & 0xFF );
		int	c  = c1 + (int)( t * (float)( c2 - c1 ) );

		if( c < 0 )
			c = 0;
		if( c > 255 )
			c = 255;

		out->rgba |= ( (unsigned int)c ) << ( i * 8 );
	}
}
#endif

#if PSP_CLIP_INTERSECT_VFPU
/*
===============
PSP_ClipIntersectVFPU

The scalar distance/divide path is deliberately left in PSP_ClipIntersect.
This helper only replaces the five float attribute lerps and four byte colour
lerps after t has been clamped. pspVert_t is 24 bytes, so all input loads use
ulv.q.  Results stage through aligned locals: a q store at out->x would write
four bytes beyond the struct, and a q store at out->u would overwrite rgba.

vuc2i/vi2f produces one float lane per RGBA byte. vf2iz with scale 23 and
vi2uc restore the exact fixed-point representation that preserves C's
toward-zero byte conversion. t is already clamped, so every colour lerp is
inside the source endpoints and vi2uc's natural saturation is sufficient.
===============
*/
static void PSP_ClipIntersectVFPU( const pspVert_t *v1, const pspVert_t *v2,
                                   pspVert_t *out, float t )
{
	float	xyz[ 4 ] __attribute__((aligned(16)));
	float	uv[ 4 ] __attribute__((aligned(16)));
	unsigned int	rgba;

	__asm__ volatile (
		"ulv.q\tR000, %[v1rgba]\n"
		"ulv.q\tR001, %[v2rgba]\n"
		"ulv.q\tR002, %[v1uv]\n"
		"ulv.q\tR003, %[v2uv]\n"
		"lv.s\tS400, %[t]\n"

		"vmov.q\tR100, R000[y,z,w,1]\n"
		"vmov.q\tR101, R001[y,z,w,1]\n"
		"vsub.t\tR102, R101, R100\n"
		"vscl.t\tR102, R102, S400\n"
		"vadd.t\tR102, R102, R100\n"
		"vsub.p\tR103, R003, R002\n"
		"vscl.p\tR103, R103, S400\n"
		"vadd.p\tR103, R103, R002\n"

		"vuc2ifs.s\tR200, S000\n"
		"vuc2ifs.s\tR201, S001\n"
		"vi2f.q\tR200, R200, 23\n"
		"vi2f.q\tR201, R201, 23\n"
		"vsub.q\tR202, R201, R200\n"
		"vscl.q\tR202, R202, S400\n"
		"vadd.q\tR202, R202, R200\n"
		"vf2iz.q\tR202, R202, 23\n"
		"vi2uc.q\tS300, R202\n"

		"usv.q\tR102, %[xyz]\n"
		"usv.q\tR103, %[uv]\n"
		"mfv\t%[rgba], S300\n"
		: [xyz] "=m"( xyz[ 0 ] ), [uv] "=m"( uv[ 0 ] ),
		  [rgba] "=r"( rgba )
		: [v1rgba] "m"( v1->rgba ), [v2rgba] "m"( v2->rgba ),
		  [v1uv] "m"( v1->u ), [v2uv] "m"( v2->u ),
		  [t] "m"( t )
		: "memory", "$f0", "$f1", "$f2", "$f3"
	);

	out->x = xyz[ 0 ];
	out->y = xyz[ 1 ];
	out->z = xyz[ 2 ];
	out->u = uv[ 0 ];
	out->v = uv[ 1 ];
	out->rgba = rgba;
}

static void PSP_ClipIntersect( const ScePspFVector4 *plane, const pspVert_t *v1,
                               const pspVert_t *v2, pspVert_t *out )
{
	float	dx = v2->x - v1->x;
	float	dy = v2->y - v1->y;
	float	dz = v2->z - v1->z;
	float	top, bottom, t;

	top    = PSP_PlaneDistance( plane, v1 );
	bottom = plane->x * dx + plane->y * dy + plane->z * dz;

	t = ( bottom != 0.0f ) ? ( -top / bottom ) : 0.0f;

	if( t < 0.0f )
		t = 0.0f;
	if( t > 1.0f )
		t = 1.0f;

	PSP_ClipIntersectVFPU( v1, v2, out, t );
}
#else
#define PSP_ClipIntersect PSP_ClipIntersectScalar
#endif

/*
===============
PSP_ClipToPlane

Sutherland-Hodgman against one plane (clipping.cpp:424-484). Returns the
output vertex count.

Two departures from the mirror, both Session 12a:

  - The plane distance of each vertex is computed ONCE. Edge i is
    (in[i], in[i+1]), so the mirror's `p` distance is the next iteration's
    `s` distance and it pays for both. Carrying it halves the dot products,
    which is the bulk of this function.

  - The wrap is hoisted out of the loop. `( i + 1 ) % inCount` is an
    integer divide on Allegrex - there is no constant to strength-reduce
    against, inCount is a runtime value - and it ran on every edge of every
    plane. The closing edge is the only one that wraps, so it is an index
    reset and one saved distance instead.
===============
*/
static int PSP_ClipToPlane( const ScePspFVector4 *plane, const pspVert_t *in, int inCount,
                            pspVert_t *out )
{
	int	outCount = 0;
	int	i;
	float	dFirst, ds, dp;

	if( inCount < 3 )
		return 0;

	dFirst = PSP_PlaneDistance( plane, &in[ 0 ] );
	ds     = dFirst;

	for( i = 0; i < inCount; i++ )
	{
		const pspVert_t	*s = &in[ i ];
		const pspVert_t	*p;
		qboolean	sIn, pIn;

		if( i + 1 < inCount )
		{
			p  = &in[ i + 1 ];
			dp = PSP_PlaneDistance( plane, p );
		}
		else
		{
			// closing edge: back to vertex 0, whose distance is already known
			p  = &in[ 0 ];
			dp = dFirst;
		}

		sIn = ( ds >= 0.0f ) ? qtrue : qfalse;
		pIn = ( dp >= 0.0f ) ? qtrue : qfalse;

		if( outCount >= PSP_CLIP_WORK_VERTS - 1 )
			break;

		if( sIn )
		{
			if( pIn )
				out[ outCount++ ] = *p;
			else
				PSP_ClipIntersect( plane, s, p, &out[ outCount++ ] );
		}
		else if( pIn )
		{
			PSP_ClipIntersect( plane, s, p, &out[ outCount++ ] );
			out[ outCount++ ] = *p;
		}

		ds = dp;
	}

	return outCount;
}

/*
===============
PSP_ClipTriangle

Clips one triangle against all four planes and writes the result out as a
triangle LIST (fan expanded), not a fan: the caller is accumulating into one
array that gets a single sceGuDrawArray, and a fan cannot be concatenated.

`mask` is the OR of the three corners' outcodes, which the caller has
already computed to decide that this triangle needs clipping at all. A clear
bit means all three corners are inside that plane, and every vertex the
earlier planes introduce is a convex combination of two of them - a
half-space is convex, so those cannot leave it either. Such a plane is
therefore provably an identity pass, and skipping it removes both its
distance work and its whole-polygon copy. Most clipped triangles cross one
plane of four; the mirror runs all four unconditionally
(clipping.cpp:486-520).

Returns the number of vertices written, always a multiple of 3, at most
PSP_CLIP_MAX_OUT.
===============
*/
static int PSP_ClipTriangle( const pspVert_t *tri, int mask, pspVert_t *out )
{
	pspVert_t	work[ 2 ][ PSP_CLIP_WORK_VERTS ];
	const pspVert_t	*src = tri;
	int		count = 3;
	int		buf = 0;
	int		i, written = 0;

	for( i = 0; i < PSP_CLIP_PLANES; i++ )
	{
		if( !( mask & ( 1 << i ) ) )
			continue;

		count = PSP_ClipToPlane( &pspClipPlane[ i ], src, count, work[ buf ] );

		if( count < 3 )
			return 0;

		src = work[ buf ];
		buf ^= 1;
	}

	for( i = 2; i < count; i++ )
	{
		out[ written++ ] = src[ 0 ];
		out[ written++ ] = src[ i - 1 ];
		out[ written++ ] = src[ i ];
	}

	return written;
}

/*
===========================================================================
Indexed draws - qglDrawElements
===========================================================================
*/

/*
===============
PSP_DrawElements

qglDrawElements, which R_DrawElements (tr_shade.c:176-182) reaches only
when r_primitives is 2. It is 2 because code/sys/sys_psp.c injects it at
boot: r_primitives 0 means "auto", and auto resolves to 1 -
R_DrawStripElements driving qglArrayElement one vertex at a time -
whenever qglLockArraysEXT is NULL, which it is on this platform
permanently.

Upstream's glIndex_t is 32-bit (renderergl1/tr_local.h:43-44) and the GE
handles 8- and 16-bit indices only, so they are narrowed on the way
through. SHADER_MAX_VERTEXES is 1000, three orders of magnitude clear of
the 16-bit ceiling.
===============
*/
void PSP_DrawElements( GLenum mode, GLsizei count, GLenum type, const GLvoid *indices )
{
	const unsigned int	*src = (const unsigned int *)indices;
	pspVert_t		*verts;
	pspVert_t		*clipVerts = NULL;
	unsigned short		*idx;
	int			vertCount, vertBytes, idxBytes, clipBytes = 0;
	int			insideIndices = 0, clipTris = 0, clipWritten = 0;
	qboolean		clipRequested, clip, cacheHit = qfalse;
	int			i;

	if( !pspGuReady || !src || count <= 0 )
		return;

	if( mode != GL_TRIANGLES || !pspVertexArray.enabled || !pspVertexArray.ptr )
		return;

	/*
	 glIndex_t is renderer-private (renderergl1/tr_local.h:43-44 - 32-bit,
	 GL_INDEX_TYPE GL_UNSIGNED_INT) and this file deliberately includes
	 only tr_common.h, so the width is checked rather than assumed. If
	 upstream ever narrows it, this says so instead of reading the indices
	 at the wrong stride.
	*/
	if( type != GL_UNSIGNED_INT )
	{
		ri.Printf( PRINT_WARNING, "PSP_DrawElements: unexpected index type 0x%04X\n", (unsigned int)type );
		return;
	}

	clipRequested = PSP_ClipActive();

	cacheHit = pspBatchCache.valid &&
		pspBatchCache.vertexPtr == pspVertexArray.ptr &&
		pspBatchCache.vertexStride == pspVertexArray.stride &&
		pspBatchCache.indices == src &&
		pspBatchCache.count == count &&
		pspBatchCache.clipRequested == clipRequested;

	if( cacheHit )
	{
		vertCount     = pspBatchCache.vertCount;
		clip          = pspBatchCache.clip;
		idx           = pspBatchCache.packedIndices;
		insideIndices = pspBatchCache.insideIndices;
		clipTris      = pspBatchCache.clipTris;
	}
	else
	{
		/*
		 The GE indexes into the vertex array we are about to build, so it has
		 to cover every index in the batch. Upstream does not tell us
		 tess.numVertexes here, and one pass over the indices is cheaper than
		 plumbing it down through a vtable that has no slot for it.
		*/
		vertCount = 0;

		Sys_PSP_RenderProfileCount( PSP_RPROF_DRAW_SCAN );
		for( i = 0; i < count; i++ )
		{
			if( (int)src[ i ] > vertCount )
				vertCount = (int)src[ i ];
		}

		vertCount++;

		if( vertCount > 65536 )
		{
			ri.Printf( PRINT_WARNING, "PSP_DrawElements: %d vertices exceeds 16-bit indices\n", vertCount );
			return;
		}

		clip = clipRequested;
		if( clip && vertCount > PSP_CLIP_MAX_VERTS )
			clip = qfalse;			// wider than the outcode scratch; draw it unclipped
	}

	/*
	 One spare vertex past the end: PSP_VertexOutcode's ulv.q reads 16 bytes
	 from a 24-byte struct, so the last vertex would otherwise read four
	 bytes off the end of the allocation.
	*/
	vertBytes = ( vertCount + 1 ) * (int)sizeof( pspVert_t );
	idxBytes  = ( count * (int)sizeof( unsigned short ) + 3 ) & ~3;

	verts = (pspVert_t *)PSP_ArenaTake( vertBytes );
	if( !cacheHit )
		idx = (unsigned short *)PSP_ArenaTake( idxBytes );

	if( !verts || ( !cacheHit && !idx ) )
	{
		pspSkippedDraws++;
		return;
	}

	/*
	 Both buffers are ordinary cached memory now (Session 10 arena), so each
	 one is written back immediately before the sceGuDrawArray that reads
	 it. The clipper below also READS verts back, which is exactly what this
	 change makes cheap - it used to pay uncached latency per access.
	*/
	Sys_PSP_RenderProfileBegin( PSP_RPROF_DRAW_PACK );
	for( i = 0; i < vertCount; i++ )
	{
		const float	*xyz = (const float *)( pspVertexArray.ptr + (size_t)i * pspVertexArray.stride );

		if( pspTexCoordArray.enabled && pspTexCoordArray.ptr )
		{
			const float *st = (const float *)( pspTexCoordArray.ptr + (size_t)i * pspTexCoordArray.stride );

			verts[ i ].u = st[ 0 ];
			verts[ i ].v = st[ 1 ];
		}
		else
		{
			verts[ i ].u = 0.0f;
			verts[ i ].v = 0.0f;
		}

		if( pspColorArray.enabled && pspColorArray.ptr )
			verts[ i ].rgba = *(const unsigned int *)( pspColorArray.ptr + (size_t)i * pspColorArray.stride );
		else
			verts[ i ].rgba = pspCurrentColor;

		verts[ i ].x = xyz[ 0 ];
		verts[ i ].y = xyz[ 1 ];
		verts[ i ].z = xyz[ 2 ];
	}
	Sys_PSP_RenderProfileEnd( PSP_RPROF_DRAW_PACK );

	if( !clip )
	{
		if( !cacheHit )
		{
			Sys_PSP_RenderProfileBegin( PSP_RPROF_DRAW_INDEX );
			for( i = 0; i < count; i++ )
				idx[ i ] = (unsigned short)src[ i ];
			Sys_PSP_RenderProfileEnd( PSP_RPROF_DRAW_INDEX );
		}

		if( !cacheHit )
		{
			pspBatchCache.vertexPtr     = pspVertexArray.ptr;
			pspBatchCache.vertexStride  = pspVertexArray.stride;
			pspBatchCache.indices       = src;
			pspBatchCache.count         = count;
			pspBatchCache.clipRequested = clipRequested;
			pspBatchCache.clip          = qfalse;
			pspBatchCache.vertCount     = vertCount;
			pspBatchCache.insideIndices = count;
			pspBatchCache.clipTris      = 0;
			pspBatchCache.packedIndices = idx;
			pspBatchCache.valid         = qtrue;
		}

		Sys_PSP_RenderProfileCount( PSP_RPROF_DRAW_SUBMIT );
		PSP_ArenaFlush( verts, vertBytes );
		PSP_ArenaFlush( idx, idxBytes );

		sceGuDrawArray( GU_TRIANGLES, PSP_VTYPE_BASE | GU_INDEX_16BIT, count, idx, verts );
		return;
	}

	/*
	 Outcode per VERTEX, not per triangle-corner. This is the whole reason
	 the clipper lives behind qglDrawElements rather than inside the
	 renderer: a tess batch has far more indices than vertices, and the
	 mirror pays for all of them.
	*/
	if( !cacheHit )
	{
		PSP_LoadClipPlanesVFPU();

		Sys_PSP_RenderProfileBegin( PSP_RPROF_DRAW_OUTCODE );
		for( i = 0; i < vertCount; i++ )
			pspBatchCache.outcode[ i ] = (byte)PSP_VertexOutcode( &verts[ i ] );
		Sys_PSP_RenderProfileEnd( PSP_RPROF_DRAW_OUTCODE );

		// Pass 1: keep wholly-inside triangles indexed, count the rest.
		Sys_PSP_RenderProfileBegin( PSP_RPROF_DRAW_CLASSIFY );
		for( i = 0; i + 2 < count; i += 3 )
		{
			unsigned int	a = src[ i ], b = src[ i + 1 ], c = src[ i + 2 ];

			if( !( pspBatchCache.outcode[ a ] | pspBatchCache.outcode[ b ] | pspBatchCache.outcode[ c ] ) )
			{
				idx[ insideIndices++ ] = (unsigned short)a;
				idx[ insideIndices++ ] = (unsigned short)b;
				idx[ insideIndices++ ] = (unsigned short)c;
			}
			else
			{
				clipTris++;
			}
		}
		Sys_PSP_RenderProfileEnd( PSP_RPROF_DRAW_CLASSIFY );

		pspBatchCache.vertexPtr     = pspVertexArray.ptr;
		pspBatchCache.vertexStride  = pspVertexArray.stride;
		pspBatchCache.indices       = src;
		pspBatchCache.count         = count;
		pspBatchCache.clipRequested = clipRequested;
		pspBatchCache.clip          = qtrue;
		pspBatchCache.vertCount     = vertCount;
		pspBatchCache.insideIndices = insideIndices;
		pspBatchCache.clipTris      = clipTris;
		pspBatchCache.packedIndices = idx;
		pspBatchCache.valid         = qtrue;
	}

	if( clipTris )
	{
		clipBytes = clipTris * PSP_CLIP_MAX_OUT * (int)sizeof( pspVert_t );
		clipVerts = (pspVert_t *)PSP_ArenaTake( clipBytes );

		if( !clipVerts )
		{
			// Not enough arena left for the worst case. Drop the clipped
			// remainder rather than the whole surface, and say so.
			pspSkippedDraws++;
			clipTris = 0;
		}
	}

	if( clipTris && clipVerts )
	{
		Sys_PSP_RenderProfileBegin( PSP_RPROF_DRAW_CLIP );
		for( i = 0; i + 2 < count; i += 3 )
		{
			unsigned int	a = src[ i ], b = src[ i + 1 ], c = src[ i + 2 ];
			pspVert_t	tri[ 3 ];
			int		mask = pspBatchCache.outcode[ a ] | pspBatchCache.outcode[ b ] | pspBatchCache.outcode[ c ];

			if( !mask )
				continue;

			tri[ 0 ] = verts[ a ];
			tri[ 1 ] = verts[ b ];
			tri[ 2 ] = verts[ c ];

			// the mask says which planes can actually cut this triangle;
			// the other passes are identity (see PSP_ClipTriangle)
			clipWritten += PSP_ClipTriangle( tri, mask, clipVerts + clipWritten );
		}

		pspClippedTris += clipTris;
		Sys_PSP_RenderProfileEnd( PSP_RPROF_DRAW_CLIP );
	}

	if( insideIndices )
	{
		// Only the indices pass 1 kept, for the same reason as clipVerts
		// below. verts is flushed whole: the kept indices can reference any
		// vertex in the batch. If insideIndices is 0 the GE never reads
		// verts at all, which is why this flush lives inside the branch.
		Sys_PSP_RenderProfileCount( PSP_RPROF_DRAW_SUBMIT );
		PSP_ArenaFlush( verts, vertBytes );
		PSP_ArenaFlush( idx, insideIndices * (int)sizeof( unsigned short ) );

		sceGuDrawArray( GU_TRIANGLES, PSP_VTYPE_BASE | GU_INDEX_16BIT,
			insideIndices, idx, verts );
	}

	if( clipWritten )
	{
		// Only the vertices the clipper actually emitted, not the worst-case
		// allocation - the tail is untouched and flushing it is pure cost.
		Sys_PSP_RenderProfileCount( PSP_RPROF_DRAW_SUBMIT );
		PSP_ArenaFlush( clipVerts, clipWritten * (int)sizeof( pspVert_t ) );

		sceGuDrawArray( GU_TRIANGLES, PSP_VTYPE_BASE, clipWritten, NULL, clipVerts );
	}
}

/*
===========================================================================
IMMEDIATE MODE

Session 7. Not optional, and not only for debug paths: the SKYBOX is
immediate mode. DrawSkySide (renderergl1/tr_sky.c:376-393) is
qglBegin(GL_TRIANGLE_STRIP) + qglTexCoord2fv + qglVertex3fv, so with these
stubbed every map renders with a hole where the sky is.

The full set of callers in this tree:

  tr_sky.c:384        GL_TRIANGLE_STRIP  skybox                    - required
  tr_shade.c:70..131  GL_TRIANGLE_STRIP  R_DrawStripElements       - unreachable
                                         at r_primitives 2, must not crash
  tr_shade.c:299      GL_LINES           r_shownormals
  tr_shadows.c:82     GL_TRIANGLE_STRIP  shadow volumes            - no-op at
                                         stencilBits 0
  tr_backend.c:774    GL_QUADS           RE_StretchRaw (cinematics)
  tr_backend.c:992    GL_QUADS           RB_ShowImages
  tr_main.c:1307,1317 GL_POLYGON         r_debugSurface

GL_QUADS has NO GE equivalent and is the one that bites: it has to be
expanded to two triangles per quad or RE_StretchRaw draws nothing at all.
===========================================================================
*/

#define PSP_IMM_MAX_VERTS	2048

/*
 One slot of slack past the end: PSP_VertexOutcode's ulv.q reads 16 bytes
 out of a 24-byte vertex, so testing the last used vertex reads four bytes
 into the slack rather than off the end of the array.
*/
static pspVert_t	pspImmVerts[ PSP_IMM_MAX_VERTS + 1 ];
static int		pspImmCount;
static int		pspImmPrim;		// GU_* primitive, or -1 for GL_QUADS
static qboolean		pspImmActive;
static qboolean		pspImmOverflowed;
static float		pspImmU, pspImmV;

#define PSP_IMM_QUADS	(-1)

void PSP_DrawBegin( GLenum mode )
{
	pspImmCount      = 0;
	pspImmOverflowed = qfalse;
	pspImmActive     = qtrue;
	pspImmU          = 0.0f;
	pspImmV          = 0.0f;

	switch( mode )
	{
		case GL_TRIANGLES:		pspImmPrim = GU_TRIANGLES;      break;
		case GL_TRIANGLE_STRIP:		pspImmPrim = GU_TRIANGLE_STRIP; break;
		case GL_TRIANGLE_FAN:		pspImmPrim = GU_TRIANGLE_FAN;   break;

		// The GE rasterises convex fans identically to GL_POLYGON, and
		// every qglBegin(GL_POLYGON) in this tree (tr_main.c:1307,1317,
		// r_debugSurface) feeds it a convex face.
		case GL_POLYGON:		pspImmPrim = GU_TRIANGLE_FAN;   break;

		case GL_LINES:			pspImmPrim = GU_LINES;          break;
		case GL_QUADS:			pspImmPrim = PSP_IMM_QUADS;     break;

		default:
			pspImmPrim   = GU_TRIANGLES;
			pspImmActive = qfalse;
			ri.Printf( PRINT_WARNING, "PSP_DrawBegin: unsupported primitive 0x%04X\n",
				(unsigned int)mode );
			break;
	}
}

void PSP_DrawImmTexCoord2f( GLfloat s, GLfloat t )
{
	pspImmU = s;
	pspImmV = t;
}

void PSP_DrawImmVertex3f( GLfloat x, GLfloat y, GLfloat z )
{
	pspVert_t	*v;

	if( !pspImmActive )
		return;

	if( pspImmCount >= PSP_IMM_MAX_VERTS )
	{
		if( !pspImmOverflowed )
		{
			pspImmOverflowed = qtrue;
			ri.Printf( PRINT_WARNING, "PSP_DrawImmVertex3f: more than %d vertices in one qglBegin\n",
				PSP_IMM_MAX_VERTS );
		}
		return;
	}

	v = &pspImmVerts[ pspImmCount++ ];

	v->u    = pspImmU;
	v->v    = pspImmV;
	v->rgba = pspCurrentColor;
	v->x    = x;
	v->y    = y;
	v->z    = z;
}

/*
===============
PSP_DrawArrayElement

qglArrayElement: pull vertex i out of the currently bound arrays and append
it. R_DrawStripElements is its only caller and r_primitives 2 keeps that
path out of the frame, but it has to be correct rather than absent - a
no-op here is a silently empty draw, which is exactly the failure mode that
cost Session 6 two hardware rounds.
===============
*/
void PSP_DrawArrayElement( GLint i )
{
	const float	*xyz;

	if( !pspImmActive || !pspVertexArray.enabled || !pspVertexArray.ptr )
		return;

	xyz = (const float *)( pspVertexArray.ptr + (size_t)i * pspVertexArray.stride );

	if( pspTexCoordArray.enabled && pspTexCoordArray.ptr )
	{
		const float *st = (const float *)( pspTexCoordArray.ptr + (size_t)i * pspTexCoordArray.stride );

		pspImmU = st[ 0 ];
		pspImmV = st[ 1 ];
	}

	if( pspColorArray.enabled && pspColorArray.ptr )
		pspCurrentColor = *(const unsigned int *)( pspColorArray.ptr + (size_t)i * pspColorArray.stride );

	PSP_DrawImmVertex3f( xyz[ 0 ], xyz[ 1 ], xyz[ 2 ] );
}

/*
 Enumerate the triangles of an accumulated primitive, preserving winding.
 Returns qfalse once k is past the end.
*/
static qboolean PSP_ImmTriangle( int prim, int total, int k, int *a, int *b, int *c )
{
	switch( prim )
	{
		case GU_TRIANGLES:
			if( ( k * 3 ) + 2 >= total )
				return qfalse;
			*a = k * 3; *b = k * 3 + 1; *c = k * 3 + 2;
			return qtrue;

		case GU_TRIANGLE_STRIP:
			if( k + 2 >= total )
				return qfalse;
			// Odd triangles have reversed winding in a strip; swap the
			// first two so the emitted list keeps one consistent winding.
			if( k & 1 )
			{
				*a = k + 1; *b = k; *c = k + 2;
			}
			else
			{
				*a = k; *b = k + 1; *c = k + 2;
			}
			return qtrue;

		case GU_TRIANGLE_FAN:
			if( k + 2 >= total )
				return qfalse;
			*a = 0; *b = k + 1; *c = k + 2;
			return qtrue;

		case PSP_IMM_QUADS:
			// Two triangles per quad: 0,1,2 and 0,2,3.
			{
				int	q = k / 2;
				int	base = q * 4;

				if( base + 3 >= total )
					return qfalse;

				if( k & 1 )
				{
					*a = base; *b = base + 2; *c = base + 3;
				}
				else
				{
					*a = base; *b = base + 1; *c = base + 2;
				}
				return qtrue;
			}

		default:
			return qfalse;
	}
}

void PSP_DrawEnd( void )
{
	int		bytes;
	pspVert_t	*out;
	int		written = 0;
	int		k, a, b, c;
	qboolean	clip;
	qboolean	needsClip = qfalse;

	if( !pspImmActive )
		return;

	pspImmActive = qfalse;

	if( pspImmCount < 2 )
		return;

	clip = PSP_ClipActive();

	/*
	 GU_LINES is never clipped: r_shownormals is the only source and a
	 wrongly clipped debug line is not worth a code path.
	*/
	if( pspImmPrim == GU_LINES )
		clip = qfalse;

	if( clip )
	{
		PSP_LoadClipPlanesVFPU();

		for( k = 0; k < pspImmCount; k++ )
		{
			if( PSP_VertexOutcode( &pspImmVerts[ k ] ) )
			{
				needsClip = qtrue;
				break;
			}
		}
	}

	/*
	 Wholly inside, or clipping off: hand the GE the primitive it was
	 given. A strip stays a strip, which is a third of the vertices a
	 triangle list would cost.
	*/
	if( !needsClip && pspImmPrim != PSP_IMM_QUADS )
	{
		bytes = pspImmCount * (int)sizeof( pspVert_t );
		out   = (pspVert_t *)PSP_ArenaTake( bytes );

		if( !out )
		{
			pspSkippedDraws++;
			return;
		}

		Com_Memcpy( out, pspImmVerts, bytes );

		PSP_ArenaFlush( out, bytes );

		sceGuDrawArray( pspImmPrim, PSP_VTYPE_BASE, pspImmCount, NULL, out );
		return;
	}

	/*
	 Otherwise expand to a triangle list. This is also the only path
	 GL_QUADS can take - the GE has no quad primitive at all.
	*/
	{
		int	maxTris = pspImmCount;		// generous upper bound for every prim above
		int	perTri  = needsClip ? PSP_CLIP_MAX_OUT : 3;

		bytes = maxTris * perTri * (int)sizeof( pspVert_t );
		out   = (pspVert_t *)PSP_ArenaTake( bytes );

		if( !out )
		{
			pspSkippedDraws++;
			return;
		}
	}

	for( k = 0; PSP_ImmTriangle( pspImmPrim, pspImmCount, k, &a, &b, &c ); k++ )
	{
		pspVert_t	tri[ 3 ];

		tri[ 0 ] = pspImmVerts[ a ];
		tri[ 1 ] = pspImmVerts[ b ];
		tri[ 2 ] = pspImmVerts[ c ];

		if( needsClip )
		{
			/*
			 All four planes. Unlike PSP_DrawElements this path has no
			 outcode pass to narrow it with - immediate mode has no
			 vertex array to classify once and index into - so every
			 triangle is clipped against everything, as before. Giving
			 this path outcodes is a separate change with its own
			 measurement.
			*/
			written += PSP_ClipTriangle( tri, 0xF, out + written );
			pspClippedTris++;
		}
		else
		{
			out[ written++ ] = tri[ 0 ];
			out[ written++ ] = tri[ 1 ];
			out[ written++ ] = tri[ 2 ];
		}
	}

	if( written )
	{
		PSP_ArenaFlush( out, written * (int)sizeof( pspVert_t ) );

		sceGuDrawArray( GU_TRIANGLES, PSP_VTYPE_BASE, written, NULL, out );
	}
}

/*
===============
PSP_DrawMemReport
===============
*/
void PSP_DrawMemReport( void )
{
	// Peak here is since the last frame report, not since boot - see
	// PSP_DrawArenaPeakKB.
	ri.Printf( PRINT_ALL, "PSP vertex arena: %d KB, peak %d KB, %d draws skipped so far\n",
		pspArenaBytes / 1024, pspArenaPeak / 1024,
		pspSkippedTotal + pspSkippedDraws );
	ri.Printf( PRINT_ALL, "PSP clipper: %s, %d triangles clipped last frame\n",
		( r_pspClip && r_pspClip->integer ) ? "on" : "off", pspClippedPrev );
	ri.Printf( PRINT_ALL, "PSP fast texcoords: %s\n",
		PSP_DrawFastTexCoords() ? "on" : "off" );
}
