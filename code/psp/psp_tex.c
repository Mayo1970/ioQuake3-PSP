/*
===========================================================================
PSP port - code/psp/psp_tex.c

Session 6. Texture objects: allocation, RGBA8888 -> 16-bit conversion,
swizzling, cache discipline, and the GU state a bind has to emit.

See psp_tex.h for the scope decisions this session was planned against
(main RAM only, 16-bit formats only, no CLUT).

Three GE facts shape everything below and are worth stating once:

  1. There is no "upload". sceGuTexImage points the GE at memory and the
     GE DMA-reads it from there for the life of the binding. So the
     conversion result has to stay allocated, and the D-cache has to be
     written back after the CPU fills it - unlike display-list memory,
     which sceGuStart already maps through the uncached mirror.
  2. sceGuTexMode's swizzle flag is per TEXTURE, not per level. Either
     every level is swizzled or none is, which is what caps the mip chain
     below.
  3. tbw (texture buffer width) is block-aligned - 16 bytes, i.e. 8 pixels
     at 16 bpp - and is a separate number from the visible width, exactly
     like the framebuffer's 512-vs-480 stride. Levels narrower than 8
     pixels get padded rather than special-cased.
===========================================================================
*/

#include "psp_tex.h"
#include "psp_gu.h"
#include "psp_pool.h"

#include <malloc.h>
#include <string.h>

#include <psputils.h>
#include <vram.h>

/*
 Where a mip level's memory came from. Session 11d: levels no longer come
 from the newlib heap by default, because that heap was measured with 3.3 MB
 free and a largest block under 1 KB while 128 KB uploads failed (psp_pool.h).

 Order of preference, and the reason for it:

   POOL   the 4 MB volatile partition. Textures are its only customer, so
          its fragmentation is the texture set's own, and it is memory the
          port was otherwise not using.
   VRAM   whatever libpspvram has left after the three framebuffers. Small
          - ~1 MB - and the GE samples it faster, though Session 11c
          measured geSync at ~0 ms, so this is memory relief and not speed.
   HEAP   the old path, kept as the fallback so a failed volatile lock
          degrades to Session 11c behaviour instead of a black screen.
*/
#define PSP_TEXMEM_HEAP	0
#define PSP_TEXMEM_POOL	1
#define PSP_TEXMEM_VRAM	2

typedef struct {
	void		*level[PSP_TEX_MAX_LEVELS];
	short		w[PSP_TEX_MAX_LEVELS];		// visible width, pixels
	short		h[PSP_TEX_MAX_LEVELS];
	short		tbw[PSP_TEX_MAX_LEVELS];	// allocated width, pixels
	unsigned char	where[PSP_TEX_MAX_LEVELS];	// PSP_TEXMEM_*
	int		bytes;				// total allocated for this texture

	unsigned char	psm;				// GU_PSM_5650 or GU_PSM_4444
	unsigned char	maxLevel;			// highest level actually stored, 0..7
	unsigned char	swizzled;
	unsigned char	used;

	unsigned char	minFilter, magFilter;
	unsigned char	wrapS, wrapT;
} pspTexture_t;

static pspTexture_t	pspTextures[ PSP_MAX_TEXTURES ];
static GLuint		pspCurrentTexture;		// GL name, 0 == none

static int		pspTexTotalBytes;
static int		pspTexCount;
static int		pspTexSwizzledCount;

static int		pspTexBytesBySource[ 3 ];	// indexed by PSP_TEXMEM_*

/*
===============
PSP_TexAllocLevel

Pool, then VRAM, then heap - see the PSP_TEXMEM_* note above. Returns the
pointer and reports which pool it came from, because free() has to match.
===============
*/
static void *PSP_TexAllocLevel( int bytes, unsigned char *where )
{
	void	*p;

	p = PSP_PoolAlloc( (size_t)bytes );
	if( p )
	{
		*where = PSP_TEXMEM_POOL;
		return p;
	}

	// vramalloc already returns an absolute pointer, which is what the CPU
	// writes through and what sceGuTexImage wants (vram.h). No vrelptr here -
	// that conversion belongs to the framebuffer calls in psp_glimp.c.
	p = vramalloc( (size_t)bytes );
	if( p )
	{
		*where = PSP_TEXMEM_VRAM;
		return p;
	}

	p = memalign( 16, bytes );
	*where = PSP_TEXMEM_HEAP;

	return p;
}

static void PSP_TexFreeLevel( void *p, unsigned char where )
{
	switch( where )
	{
		case PSP_TEXMEM_POOL:	PSP_PoolFree( p );	break;
		case PSP_TEXMEM_VRAM:	vfree( p );		break;
		default:		free( p );		break;
	}
}

/*
=================================================================
Slot management
=================================================================
*/

static pspTexture_t *PSP_TexSlot( GLuint name )
{
	if( name == 0 || name > PSP_MAX_TEXTURES )
		return NULL;

	return &pspTextures[ name - 1 ];
}

/*
===============
PSP_TexGenName

qglGenTextures. 0 is reserved by GL and must never be issued.
===============
*/
GLuint PSP_TexGenName( void )
{
	int	i;

	for( i = 0; i < PSP_MAX_TEXTURES; i++ )
	{
		pspTexture_t	*tex = &pspTextures[ i ];

		if( tex->used )
			continue;

		Com_Memset( tex, 0, sizeof( *tex ) );

		tex->used      = 1;
		tex->psm       = GU_PSM_5650;
		tex->minFilter = GU_LINEAR;
		tex->magFilter = GU_LINEAR;
		tex->wrapS     = GU_REPEAT;
		tex->wrapT     = GU_REPEAT;

		pspTexCount++;

		return (GLuint)( i + 1 );
	}

	ri.Printf( PRINT_WARNING, "PSP_TexGenName: out of texture slots (%d)\n", PSP_MAX_TEXTURES );

	return 0;
}

static void PSP_TexFreeLevels( pspTexture_t *tex )
{
	int	i;

	for( i = 0; i < PSP_TEX_MAX_LEVELS; i++ )
	{
		if( tex->level[ i ] )
		{
			int	levelBytes = tex->tbw[ i ] * tex->h[ i ] * 2;

			PSP_TexFreeLevel( tex->level[ i ], tex->where[ i ] );
			pspTexBytesBySource[ tex->where[ i ] ] -= levelBytes;

			tex->level[ i ] = NULL;
			tex->where[ i ] = PSP_TEXMEM_HEAP;
		}
	}

	pspTexTotalBytes -= tex->bytes;
	tex->bytes    = 0;
	tex->maxLevel = 0;
}

/*
===============
PSP_TexDelete
===============
*/
void PSP_TexDelete( GLuint name )
{
	pspTexture_t	*tex = PSP_TexSlot( name );

	if( !tex || !tex->used )
		return;

	if( tex->swizzled )
		pspTexSwizzledCount--;

	PSP_TexFreeLevels( tex );

	tex->used = 0;
	pspTexCount--;

	if( pspCurrentTexture == name )
		pspCurrentTexture = 0;
}

/*
=================================================================
Conversion

GL hands us GL_RGBA / GL_UNSIGNED_BYTE - r,g,b,a in ascending memory
order. The GE's 16-bit texture formats put red in the LOW bits, so both
conversions read like the little-endian bit layouts they are and neither
needs a byte swap (the Allegrex is little-endian; unlike the Wii and PS3
ports there is no endian work anywhere in this file).
=================================================================
*/

#define PSP_CONV_5650( p )	( (unsigned short)( ( (p)[0] >> 3 ) | ( ( (p)[1] >> 2 ) << 5 ) | ( ( (p)[2] >> 3 ) << 11 ) ) )
#define PSP_CONV_4444( p )	( (unsigned short)( ( (p)[0] >> 4 ) | ( ( (p)[1] >> 4 ) << 4 ) | ( ( (p)[2] >> 4 ) << 8 ) | ( ( (p)[3] >> 4 ) << 12 ) ) )

/*
 Destination index of pixel (x, y) within a level.

 Swizzled memory is 16-byte by 8-row blocks laid out block-row major -
 the same tiling Quake3PSP-mirror/renderer/tr_image.cpp:349 blits with
 word copies. That fast path only works for a whole level at a time;
 doing the index arithmetic per pixel instead means one code path serves
 both a full qglTexImage2D and a partial qglTexSubImage2D (the cinematic
 update at tr_backend.c:803), and this only ever runs at load time.
*/
static int PSP_TexelOffset( int x, int y, int tbw, qboolean swizzled )
{
	int	blockRow, blockCol;

	if( !swizzled )
		return y * tbw + x;

	blockRow = y >> 3;
	blockCol = x >> 3;

	return ( ( blockRow * ( tbw >> 3 ) + blockCol ) * 8 + ( y & 7 ) ) * 8 + ( x & 7 );
}

/*
 Convert an RGBA8888 rectangle into an already-allocated level, at
 (xoffset, yoffset). srcPitch is the source width in pixels.
*/
static void PSP_TexBlit( pspTexture_t *tex, int level, int xoffset, int yoffset,
                         int width, int height, const byte *src, int srcPitch )
{
	unsigned short	*dst = (unsigned short *)tex->level[ level ];
	const int	tbw = tex->tbw[ level ];
	const qboolean	swizzled = tex->swizzled ? qtrue : qfalse;
	int		x, y;

	if( !dst )
		return;

	if( tex->psm == GU_PSM_4444 )
	{
		for( y = 0; y < height; y++ )
		{
			const byte	*row = src + y * srcPitch * 4;

			for( x = 0; x < width; x++ )
				dst[ PSP_TexelOffset( xoffset + x, yoffset + y, tbw, swizzled ) ] =
					PSP_CONV_4444( row + x * 4 );
		}
	}
	else
	{
		for( y = 0; y < height; y++ )
		{
			const byte	*row = src + y * srcPitch * 4;

			for( x = 0; x < width; x++ )
				dst[ PSP_TexelOffset( xoffset + x, yoffset + y, tbw, swizzled ) ] =
					PSP_CONV_5650( row + x * 4 );
		}
	}
}

/*
 The GE DMA-reads this memory; the CPU just wrote it through the D-cache.
 Round the length up to a whole 64-byte line - a partial trailing line
 left dirty is the "renders in PPSSPP, garbage on hardware" failure, and
 PPSSPP cannot catch it because it emulates no D-cache at all.
 Allocations are memalign(16) and the levels are >= 128 bytes, so rounding
 up never reaches another live allocation's dirty data.
*/
static void PSP_TexWriteback( void *ptr, int bytes )
{
	sceKernelDcacheWritebackRange( ptr, ( bytes + 63 ) & ~63 );
}

/*
===============
PSP_TexUpload2D

qglTexImage2D. Upstream's Upload32 (renderergl1/tr_image.c:741-801) has
already resampled to a power of two, applied r_picmip, clamped to
glConfig.maxTextureSize (512, published by psp_glimp.c) and light-scaled
the pixels, then calls us once per mip level starting at 0. So there is
no scaling, no gamma and no mip generation to do here - only format,
padding, swizzle and cache.

internalFormat is upstream's own "does this image use alpha" answer
(tr_image.c:651-739): the samples==3 branch picks a GL_RGB* or
GL_LUMINANCE* token, samples==4 picks GL_RGBA* or GL_LUMINANCE_ALPHA*.
glConfig.textureCompression is TC_NONE, so the GL_COMPRESSED_* and
GL_RGB4_S3TC branches there are unreachable and are not handled.
===============
*/
void PSP_TexUpload2D( GLint level, GLenum internalFormat, GLsizei width, GLsizei height, const void *rgba )
{
	pspTexture_t	*tex = PSP_TexSlot( pspCurrentTexture );
	int		tbw, bytes;
	int		psm;

	if( !tex || !tex->used || !rgba || width <= 0 || height <= 0 )
		return;

	if( level < 0 || level >= PSP_TEX_MAX_LEVELS )
		return;		// past the GE's eight-level ceiling; drop it

	switch( internalFormat )
	{
		case GL_RGBA:
		case GL_RGBA4:
		case GL_RGBA8:
		case GL_LUMINANCE_ALPHA:
		case GL_LUMINANCE8_ALPHA8:
			psm = GU_PSM_4444;
			break;
		default:
			psm = GU_PSM_5650;
			break;
	}

	/*
	 Level 0 defines the texture. A later level arriving with a different
	 format would mean upstream changed its mind mid-chain, which it does
	 not do - but if it ever did, honouring level 0 keeps the levels
	 consistent with the single sceGuTexMode that describes all of them.

	 Levels must arrive in order and stop at the first gap. A re-upload of
	 level 0 (RE_UploadCinematic, tr_backend.c:794) discards the old chain.
	*/
	if( level == 0 )
	{
		if( tex->swizzled )
			pspTexSwizzledCount--;

		PSP_TexFreeLevels( tex );

		tex->psm = (unsigned char)psm;

		/*
		 Swizzling needs whole 16-byte x 8-row blocks. tbw padding
		 guarantees the width side; the height side is why the mip chain
		 stops below. A level 0 shorter than 8 rows cannot be swizzled at
		 all, so that texture stays linear - it costs sampling bandwidth
		 on an image too small for that to matter.

		 The multiple-of-8 test is not decoration. The swizzled offset
		 formula addresses whole blocks, so a level whose height is not a
		 multiple of 8 would compute offsets past the end of its own
		 allocation. Every level upstream produces is a power of two, so
		 this never fires - but a heap overrun on hardware is not a bug
		 worth leaving to that guarantee.
		*/
		tex->swizzled = ( height >= 8 && ( height & 7 ) == 0 ) ? 1 : 0;

		if( tex->swizzled )
			pspTexSwizzledCount++;
	}
	else
	{
		if( level != tex->maxLevel + 1 )
			return;		// out of order, or continuing past a level we refused

		/*
		 Stop the chain here rather than storing a level the swizzle
		 cannot express. Upstream keeps halving down to 1x1; the GE just
		 uses the deepest level we declared.
		*/
		if( tex->swizzled && ( height < 8 || ( height & 7 ) != 0 ) )
			return;
	}

	tbw = ( width + 7 ) & ~7;
	if( tbw < 8 )
		tbw = 8;

	bytes = tbw * height * 2;

	tex->level[ level ] = PSP_TexAllocLevel( bytes, &tex->where[ level ] );

	if( !tex->level[ level ] )
	{
		ri.Printf( PRINT_WARNING, "PSP_TexUpload2D: out of memory for %dx%d level %d (%d bytes)\n",
			width, height, level, bytes );
		return;
	}

	pspTexBytesBySource[ tex->where[ level ] ] += bytes;

	// Padding columns are sampled by nothing, but leaving them undefined
	// makes a tbw bug look like random noise instead of a black edge.
	Com_Memset( tex->level[ level ], 0, bytes );

	tex->w[ level ]   = (short)width;
	tex->h[ level ]   = (short)height;
	tex->tbw[ level ] = (short)tbw;
	tex->maxLevel     = (unsigned char)level;
	tex->bytes       += bytes;
	pspTexTotalBytes += bytes;

	PSP_TexBlit( tex, level, 0, 0, width, height, (const byte *)rgba, width );
	PSP_TexWriteback( tex->level[ level ], bytes );

	/*
	 * Level 0 uploads replace the backing store. GL_Bind may suppress the
	 * following bind because its cache still says this texture is current,
	 * but the GE still has the old sceGuTexImage pointer. Re-emit the GU
	 * texture state now so the next draw samples the newly uploaded pixels.
	 */
	if( level == 0 && pspGuReady )
		PSP_TexBind( pspCurrentTexture );

#if PSP_GFX_DEBUG
	{
		static int	dumped = 0;

		if( dumped < 8 )
		{
			const byte		*p   = (const byte *)rgba;
			const unsigned short	*out = (const unsigned short *)tex->level[ level ];

			dumped++;

			ri.Printf( PRINT_ALL, "TEXUP n=%u lvl=%d %dx%d tbw=%d psm=%d swz=%d ifmt=%04X src %02X%02X%02X%02X -> %04X\n",
				(unsigned int)pspCurrentTexture, level, width, height,
				(int)tex->tbw[ level ], (int)tex->psm, (int)tex->swizzled,
				(unsigned int)internalFormat,
				p[0], p[1], p[2], p[3], (unsigned int)out[0] );
		}
	}
#endif
}

/*
===============
PSP_TexSubImage2D

qglTexSubImage2D. Only reached from RE_StretchRaw's cinematic path
(tr_backend.c:803), which re-uploads the whole of a 256x256 scratch image
every frame it plays.
===============
*/
void PSP_TexSubImage2D( GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, const void *rgba )
{
	pspTexture_t	*tex = PSP_TexSlot( pspCurrentTexture );

	if( !tex || !tex->used || !rgba || width <= 0 || height <= 0 )
		return;

	if( level < 0 || level > tex->maxLevel || !tex->level[ level ] )
		return;

	if( xoffset < 0 || yoffset < 0 ||
	    xoffset + width > tex->w[ level ] || yoffset + height > tex->h[ level ] )
		return;

	PSP_TexBlit( tex, level, xoffset, yoffset, width, height, (const byte *)rgba, width );
	PSP_TexWriteback( tex->level[ level ], tex->tbw[ level ] * tex->h[ level ] * 2 );
}

/*
=================================================================
Sampler state

GL keeps filter and wrap per texture object; the GE keeps them as global
render state. So they are stored per texture here and emitted at bind
time - the same shape as Quake3PSP-mirror's GL_Bind
(renderer/tr_backend.cpp:69-142), which reads image->filter and
image->wrapClampMode back out at every bind.
=================================================================
*/

static void PSP_TexEmitSamplerState( const pspTexture_t *tex )
{
	int	minFilter = tex->minFilter;

	/*
	 A mipmap min-filter on a texture with one level makes the GE sample a
	 level that was never declared. Upstream sets gl_filter_min from
	 r_textureMode ("GL_LINEAR_MIPMAP_NEAREST" by default) on every
	 mipmapped image, and the mip chain here can be shorter than upstream
	 uploaded - or empty - so the downgrade is not hypothetical.
	*/
	if( tex->maxLevel == 0 )
	{
		if( minFilter == GU_NEAREST_MIPMAP_NEAREST || minFilter == GU_NEAREST_MIPMAP_LINEAR )
			minFilter = GU_NEAREST;
		else if( minFilter == GU_LINEAR_MIPMAP_NEAREST || minFilter == GU_LINEAR_MIPMAP_LINEAR )
			minFilter = GU_LINEAR;
	}

	sceGuTexFilter( minFilter, tex->magFilter );
	sceGuTexWrap( tex->wrapS, tex->wrapT );
}

/*
===============
PSP_TexParameter

qglTexParameterf/qglTexParameteri, both of which upstream only ever calls
with filter and wrap tokens. GL_TEXTURE_MAX_ANISOTROPY_EXT is unreachable
- tr_init.c leaves textureFilterAnisotropic false because psp_qgl.c
publishes an empty extension string.

R_GammaCorrect / GL_TextureMode (tr_image.c:110-120) walks every loaded
image re-issuing these while each is bound, so a parameter change on the
currently bound texture has to re-emit or the GE keeps the old sampler
state until something else binds.
===============
*/
void PSP_TexParameter( GLenum pname, GLint value )
{
	pspTexture_t	*tex = PSP_TexSlot( pspCurrentTexture );
	int		mapped;

	if( !tex || !tex->used )
		return;

	switch( pname )
	{
		case GL_TEXTURE_MIN_FILTER:
		case GL_TEXTURE_MAG_FILTER:
			switch( value )
			{
				case GL_NEAREST:		mapped = GU_NEAREST; break;
				case GL_LINEAR:			mapped = GU_LINEAR; break;
				case GL_NEAREST_MIPMAP_NEAREST:	mapped = GU_NEAREST_MIPMAP_NEAREST; break;
				case GL_LINEAR_MIPMAP_NEAREST:	mapped = GU_LINEAR_MIPMAP_NEAREST; break;
				case GL_NEAREST_MIPMAP_LINEAR:	mapped = GU_NEAREST_MIPMAP_LINEAR; break;
				case GL_LINEAR_MIPMAP_LINEAR:	mapped = GU_LINEAR_MIPMAP_LINEAR; break;
				default:			mapped = GU_LINEAR; break;
			}

			// The GE has no mipmapped magnification.
			if( pname == GL_TEXTURE_MAG_FILTER )
			{
				if( mapped >= GU_NEAREST_MIPMAP_NEAREST )
					mapped = ( mapped & 1 ) ? GU_LINEAR : GU_NEAREST;

				tex->magFilter = (unsigned char)mapped;
			}
			else
			{
				tex->minFilter = (unsigned char)mapped;
			}
			break;

		case GL_TEXTURE_WRAP_S:
		case GL_TEXTURE_WRAP_T:
			mapped = ( value == GL_REPEAT ) ? GU_REPEAT : GU_CLAMP;

			if( pname == GL_TEXTURE_WRAP_S )
				tex->wrapS = (unsigned char)mapped;
			else
				tex->wrapT = (unsigned char)mapped;
			break;

		default:
			return;
	}

	if( pspGuReady )
		PSP_TexEmitSamplerState( tex );
}

/*
===============
PSP_TexBind

qglBindTexture. GL_Bind (tr_backend.c:44-66) already suppresses redundant
binds through glState.currenttextures, so this emits unconditionally.

Name 0 is the unbind upstream does after creating an image
(tr_image.c:899). Nothing is drawn in that state, so record it and emit
nothing - leaving the previous texture's registers in place is harmless
and cheaper than describing a texture that does not exist.
===============
*/
void PSP_TexBind( GLuint name )
{
	pspTexture_t	*tex;
	int		i;

	pspCurrentTexture = name;

	if( !pspGuReady || name == 0 )
		return;

	tex = PSP_TexSlot( name );

	if( !tex || !tex->used || !tex->level[ 0 ] )
		return;

	sceGuTexMode( tex->psm, tex->maxLevel, 0, tex->swizzled );

	for( i = 0; i <= tex->maxLevel; i++ )
		sceGuTexImage( i, tex->w[ i ], tex->h[ i ], tex->tbw[ i ], tex->level[ i ] );

#if PSP_GFX_DEBUG
	{
		static int	dumped = 0;

		if( dumped < 6 )
		{
			dumped++;

			ri.Printf( PRINT_ALL, "BIND n=%u %dx%d tbw=%d psm=%d swz=%d maxlvl=%d filt %d/%d wrap %d/%d\n",
				(unsigned int)name, (int)tex->w[ 0 ], (int)tex->h[ 0 ], (int)tex->tbw[ 0 ],
				(int)tex->psm, (int)tex->swizzled, (int)tex->maxLevel,
				(int)tex->minFilter, (int)tex->magFilter, (int)tex->wrapS, (int)tex->wrapT );
		}
	}
#endif

	if( tex->maxLevel > 0 )
		sceGuTexLevelMode( GU_TEXTURE_AUTO, 0.0f );

	PSP_TexEmitSamplerState( tex );

	/*
	 The GE has a small texture cache keyed on the texture buffer address.
	 Two textures can land on the same address across a free/malloc pair,
	 so flushing at bind - not only after a CPU write - is what keeps a
	 stale page from being sampled.
	*/
	sceGuTexFlush();
}

/*
===============
PSP_TexMemReport

Session 10 tunes r_picmip, com_soundMegs and the rest against measured
peaks. This is where its texture number comes from, and printing it now
costs ten lines instead of a re-measurement session later.
===============
*/
void PSP_TexMemReport( void )
{
	int	poolUsed = 0, poolTotal = 0, poolLargest = 0;

	ri.Printf( PRINT_ALL, "PSP textures: %d images, %d KB, %d swizzled / %d linear\n",
		pspTexCount,
		pspTexTotalBytes / 1024,
		pspTexSwizzledCount,
		pspTexCount - pspTexSwizzledCount );

	PSP_PoolStats( &poolUsed, &poolTotal, &poolLargest );

	// Which pool the texture set actually landed in. If heap is non-zero the
	// other two ran out, and that - not the total - is what to re-budget.
	ri.Printf( PRINT_ALL, "PSP texmem: pool %d KB, vram %d KB, heap %d KB "
		"(pool %d/%d KB used, largest free %d KB, vram %d KB free)\n",
		pspTexBytesBySource[ PSP_TEXMEM_POOL ] / 1024,
		pspTexBytesBySource[ PSP_TEXMEM_VRAM ] / 1024,
		pspTexBytesBySource[ PSP_TEXMEM_HEAP ] / 1024,
		poolUsed / 1024, poolTotal / 1024, poolLargest / 1024,
		(int)( vmemavail() / 1024 ) );
}
