// Texture objects: allocation, RGBA8888 -> 16-bit or DXT conversion, swizzling, cache, GU state.
// The GE samples straight from this memory, so it stays allocated and must be written back.

#include "psp_tex.h"
#include "psp_gu.h"
#include "psp_pool.h"
#include "psp_dxt.h"

#include <malloc.h>
#include <string.h>

#include <psputils.h>
#include <vram.h>

// Where a level came from: the volatile pool first, then VRAM left after the framebuffers,
// then the heap, which also keeps a failed volatile lock from becoming a black screen.
#define PSP_TEXMEM_HEAP	0
#define PSP_TEXMEM_POOL	1
#define PSP_TEXMEM_VRAM	2

#define PSP_TEX_IS_DXT( psm )	( (psm) == GU_PSM_DXT1 || (psm) == GU_PSM_DXT5 )

typedef struct {
	void		*level[PSP_TEX_MAX_LEVELS];
	short		w[PSP_TEX_MAX_LEVELS];		// visible width, pixels
	short		h[PSP_TEX_MAX_LEVELS];
	short		tbw[PSP_TEX_MAX_LEVELS];	// allocated width, pixels
	unsigned char	where[PSP_TEX_MAX_LEVELS];	// PSP_TEXMEM_*
	int		bytes;				// total allocated for this texture

	unsigned char	psm;				// GU_PSM_5650, 4444, DXT1 or DXT5
	unsigned char	maxLevel;			// highest level actually stored, 0..7
	unsigned char	swizzled;
	unsigned char	used;

	unsigned char	minFilter, magFilter;
	unsigned char	wrapS, wrapT;
#ifdef MISSIONPACK
	unsigned char	clut;				// pspTexCluts row of a GU_PSM_T4 texture
#endif
} pspTexture_t;

static pspTexture_t	pspTextures[ PSP_MAX_TEXTURES ];
static GLuint		pspCurrentTexture;		// GL name, 0 == none

static int		pspTexTotalBytes;
static int		pspTexCount;
static int		pspTexSwizzledCount;

static int		pspTexBytesBySource[ 3 ];	// indexed by PSP_TEXMEM_*

// Returns which pool the level came from, because the free has to match it.
static void *PSP_TexAllocLevel( int bytes, unsigned char *where )
{
	void	*p;

	p = PSP_PoolAlloc( (size_t)bytes );
	if( p )
	{
		*where = PSP_TEXMEM_POOL;
		return p;
	}

	// vramalloc returns the absolute pointer that both the CPU and sceGuTexImage use.
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

// DXT levels keep the height a multiple of 4, so the block sizes divide exactly.
static int PSP_TexLevelBytes( int psm, int tbw, int height )
{
	if( psm == GU_PSM_DXT1 )
		return tbw * height / 2;
	if( psm == GU_PSM_DXT5 )
		return tbw * height;
#ifdef MISSIONPACK
	if( psm == GU_PSM_T4 )
		return tbw * height / 2;
#endif
	return tbw * height * 2;
}

static pspTexture_t *PSP_TexSlot( GLuint name )
{
	if( name == 0 || name > PSP_MAX_TEXTURES )
		return NULL;

	return &pspTextures[ name - 1 ];
}

// qglGenTextures. 0 is reserved by GL and must never be issued.
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
			int	levelBytes = PSP_TexLevelBytes( tex->psm, tex->tbw[ i ], tex->h[ i ] );

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

// GL hands over r,g,b,a bytes; the GE 16-bit formats put red in the low bits, so no swap.
#define PSP_CONV_5650( p )	( (unsigned short)( ( (p)[0] >> 3 ) | ( ( (p)[1] >> 2 ) << 5 ) | ( ( (p)[2] >> 3 ) << 11 ) ) )
#define PSP_CONV_4444( p )	( (unsigned short)( ( (p)[0] >> 4 ) | ( ( (p)[1] >> 4 ) << 4 ) | ( ( (p)[2] >> 4 ) << 8 ) | ( ( (p)[3] >> 4 ) << 12 ) ) )

// Swizzled levels are 16-byte x 8-row blocks, block-row major. Per-pixel index maths lets one
// path serve both a full upload and the cinematic sub-image update; it runs at load time only.
static int PSP_TexelOffset( int x, int y, int tbw, qboolean swizzled )
{
	int	blockRow, blockCol;

	if( !swizzled )
		return y * tbw + x;

	blockRow = y >> 3;
	blockCol = x >> 3;

	return ( ( blockRow * ( tbw >> 3 ) + blockCol ) * 8 + ( y & 7 ) ) * 8 + ( x & 7 );
}

// Converts an RGBA8888 rectangle into an allocated 16-bit level at (xoffset, yoffset).
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

// A partial trailing line left dirty renders fine in PPSSPP (no D-cache) and as garbage on
// hardware. Levels are 16-aligned and >= 128 bytes, so rounding up stays inside them.
static void PSP_TexWriteback( void *ptr, int bytes )
{
	sceKernelDcacheWritebackRange( ptr, ( bytes + 63 ) & ~63 );
}

#ifdef MISSIONPACK
// One-colour alpha textures (font atlases) keep 4-bit indices into a 16-step alpha ramp of their
// colour: a quarter of 4444 at the same 16 alpha levels. The GE reads the CLUT at draw time.
#define PSP_TEX_MAX_CLUTS	8

static unsigned int	pspTexCluts[ PSP_TEX_MAX_CLUTS ][ 16 ] __attribute__( ( aligned( 64 ) ) );
static int		pspTexClutCount;

// The ramp for this RGB, made on first use; -1 once every row holds another colour.
static int PSP_TexClutFor( const byte *rgb )
{
	const unsigned int	colour = rgb[ 0 ] | ( rgb[ 1 ] << 8 ) | ( rgb[ 2 ] << 16 );
	int			i;

	for( i = 0; i < pspTexClutCount; i++ )
	{
		if( ( pspTexCluts[ i ][ 0 ] & 0xFFFFFF ) == colour )
			return i;
	}
	if( pspTexClutCount == PSP_TEX_MAX_CLUTS )
		return -1;

	for( i = 0; i < 16; i++ )
		pspTexCluts[ pspTexClutCount ][ i ] = ( (unsigned int)( i * 17 ) << 24 ) | colour;
	PSP_TexWriteback( pspTexCluts[ pspTexClutCount ], sizeof( pspTexCluts[ 0 ] ) );

	return pspTexClutCount++;
}

// Two texels per byte, the left one in the low nibble; a swizzle block is 32 x 8 texels.
static int PSP_TexT4Offset( int x, int y, int tbw, qboolean swizzled )
{
	const int	bx = x >> 1;

	if( !swizzled )
		return y * ( tbw >> 1 ) + bx;

	return ( ( ( y >> 3 ) * ( tbw >> 5 ) + ( bx >> 4 ) ) * 8 + ( y & 7 ) ) * 16 + ( bx & 15 );
}

// The index is the alpha's top nibble, as PSP_CONV_4444 keeps it. The level must be zeroed.
static void PSP_TexBlitT4( pspTexture_t *tex, int level, int width, int height, const byte *src )
{
	byte		*dst = (byte *)tex->level[ level ];
	const int	tbw = tex->tbw[ level ];
	const qboolean	swizzled = tex->swizzled ? qtrue : qfalse;
	int		x, y;

	for( y = 0; y < height; y++ )
	{
		for( x = 0; x < width; x++, src += 4 )
			dst[ PSP_TexT4Offset( x, y, tbw, swizzled ) ] |= ( src[ 3 ] >> 4 ) << ( ( x & 1 ) * 4 );
	}
}

// Set while PSP_TexUpload2D takes a TA cinematic frame that is already GE-order 5650.
static qboolean	pspTexSrc5650;

// Copies a 5650 rectangle into a 5650 level. Block-aligned swizzled rectangles (every cinematic)
// move as 16-byte block rows; anything else goes texel by texel.
static void PSP_TexCopy5650( pspTexture_t *tex, int level, int xoffset, int yoffset,
                             int width, int height, const unsigned short *src )
{
	unsigned short	*dst = (unsigned short *)tex->level[ level ];
	const int	tbw = tex->tbw[ level ];
	int		x, y, row;

	if( !dst )
		return;

	if( tex->swizzled && !( ( xoffset | yoffset | width | height ) & 7 ) &&
	    !( (size_t)src & 3 ) )
	{
		const int	srcWords = width >> 1;

		for( y = 0; y < height; y += 8 )
		{
			for( x = 0; x < width; x += 8 )
			{
				unsigned int		*d = (unsigned int *)( dst +
					( ( ( yoffset + y ) >> 3 ) * ( tbw >> 3 ) + ( ( xoffset + x ) >> 3 ) ) * 64 );
				const unsigned int	*s = (const unsigned int *)( src + y * width + x );

				for( row = 0; row < 8; row++, d += 4, s += srcWords )
				{
					d[ 0 ] = s[ 0 ];
					d[ 1 ] = s[ 1 ];
					d[ 2 ] = s[ 2 ];
					d[ 3 ] = s[ 3 ];
				}
			}
		}
		return;
	}

	for( y = 0; y < height; y++ )
	{
		for( x = 0; x < width; x++ )
			dst[ PSP_TexelOffset( xoffset + x, yoffset + y, tbw, tex->swizzled ? qtrue : qfalse ) ] =
				src[ y * width + x ];
	}
}
#endif

// The Xbox port's policy: picmip'd or mipmapped art, and 2D art from 256 (opaque) or 512 (alpha).
// Cinematics never come here: they upload through qglTexImage2D and stay 16-bit.
GLenum PSP_TexChooseFormat( GLenum internalFormat, qboolean alpha, int width, int height,
	qboolean picmip, qboolean mipmap )
{
#ifdef MISSIONPACK
	// TA menu art stays loaded in a match and fills the pool, so 2D art goes DXT from r_pspDxt2D px
	// on both sides (the Xbox port uses 128). 64 keeps the smallest icons 16-bit.
	const int	minSide = ri.Cvar_Get( "r_pspDxt2D", "64", 0 )->integer;
#else
	const int	minSide = alpha ? 512 : 256;
#endif

	if( !ri.Cvar_Get( "r_pspDxt", "1", 0 )->integer || width < 8 || height < 4 )
		return internalFormat;

	if( !picmip && !mipmap && ( width < minSide || height < minSide ) )
		return internalFormat;

	return alpha ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
}

#ifdef MISSIONPACK
// Font atlases stay 16-bit: DXT blocks would step their antialiased glyph edges (Xbox port).
qboolean PSP_TexIsTextArt( const char *name )
{
	return ( !Q_stricmpn( name, "fonts/", 6 ) || !Q_stricmpn( name, "menu/art/font", 13 ) ||
		!Q_stricmp( name, "gfx/2d/bigchars" ) ) ? qtrue : qfalse;
}
#endif

// qglTexImage2D, once per level from 0. Upload32 has already resampled, picmip'd, clamped to
// 512 and light-scaled; internalFormat carries its alpha answer or PSP_TexChooseFormat's DXT.
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
		case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
			psm = GU_PSM_DXT1;
			break;
		case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
			psm = GU_PSM_DXT5;
			break;
#ifdef MISSIONPACK
		case PSP_GL_ONE_COLOUR:
			psm = GU_PSM_T4;
			break;
#endif
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

	// Level 0 defines the texture; a re-upload of it (cinematics) discards the old chain.
	if( level == 0 )
	{
		if( tex->swizzled )
			pspTexSwizzledCount--;

		PSP_TexFreeLevels( tex );

		// DXT needs whole 4x4 blocks and a tbw of at least 8; smaller images stay 16-bit.
		if( PSP_TEX_IS_DXT( psm ) && ( width < 8 || height < 4 ) )
			psm = ( psm == GU_PSM_DXT1 ) ? GU_PSM_5650 : GU_PSM_4444;
#ifdef MISSIONPACK
		if( psm == GU_PSM_T4 )
		{
			const int	clut = PSP_TexClutFor( (const byte *)rgba );

			if( clut < 0 )
				psm = GU_PSM_4444;
			else
				tex->clut = (unsigned char)clut;
		}
#endif

		tex->psm = (unsigned char)psm;

		// Swizzled offsets address whole 8-row blocks, so a shorter level would overrun.
		// DXT blocks have their own layout and never swizzle.
		tex->swizzled = ( !PSP_TEX_IS_DXT( psm ) && height >= 8 && ( height & 7 ) == 0 ) ? 1 : 0;

		if( tex->swizzled )
			pspTexSwizzledCount++;
	}
	else
	{
		if( level != tex->maxLevel + 1 )
			return;		// out of order, or continuing past a level we refused

		// Stop the chain at the first level the format cannot express; the GE uses the deepest.
		if( tex->swizzled && ( height < 8 || ( height & 7 ) != 0 ) )
			return;
		if( PSP_TEX_IS_DXT( tex->psm ) && ( width < 8 || height < 4 ) )
			return;
	}

	if( PSP_TEX_IS_DXT( tex->psm ) )
	{
		tbw = width;
	}
#ifdef MISSIONPACK
	else if( tex->psm == GU_PSM_T4 )
	{
		// 16-byte rows, which is also a swizzle block's width.
		tbw = ( width + 31 ) & ~31;
	}
#endif
	else
	{
		tbw = ( width + 7 ) & ~7;
		if( tbw < 8 )
			tbw = 8;
	}

	bytes = PSP_TexLevelBytes( tex->psm, tbw, height );

	tex->level[ level ] = PSP_TexAllocLevel( bytes, &tex->where[ level ] );

	if( !tex->level[ level ] )
	{
		ri.Printf( PRINT_WARNING, "PSP_TexUpload2D: out of memory for %dx%d level %d (%d bytes)\n",
			width, height, level, bytes );
		return;
	}

	pspTexBytesBySource[ tex->where[ level ] ] += bytes;

	tex->w[ level ]   = (short)width;
	tex->h[ level ]   = (short)height;
	tex->tbw[ level ] = (short)tbw;
	tex->maxLevel     = (unsigned char)level;
	tex->bytes       += bytes;
	pspTexTotalBytes += bytes;

	if( PSP_TEX_IS_DXT( tex->psm ) )
	{
		// r_pspDxtFast 0 brings back the least-squares refine: better colour, slower load.
		const qboolean refine = ri.Cvar_Get( "r_pspDxtFast", "1", 0 )->integer ? qfalse : qtrue;
		unsigned int countStart = Sys_PSP_CountBegin();

		PSP_DxtCompress( tex->psm == GU_PSM_DXT5 ? qtrue : qfalse, refine, (const byte *)rgba,
			width, height, tex->level[ level ] );
		Sys_PSP_CountEnd( PSP_COUNT_DXT, countStart, (unsigned int)( width * height ) );
	}
	else
	{
		// Zeroed padding makes a tbw bug a black edge instead of noise.
		Com_Memset( tex->level[ level ], 0, bytes );
#ifdef MISSIONPACK
		if( tex->psm == GU_PSM_T4 )
			PSP_TexBlitT4( tex, level, width, height, (const byte *)rgba );
		else if( pspTexSrc5650 )
			PSP_TexCopy5650( tex, level, 0, 0, width, height, (const unsigned short *)rgba );
		else
#endif
		PSP_TexBlit( tex, level, 0, 0, width, height, (const byte *)rgba, width );
	}
	PSP_TexWriteback( tex->level[ level ], bytes );

	// GL_Bind may skip the next bind of this name, but the GE still holds the old pointer.
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

// qglTexSubImage2D, reached only from RE_StretchRaw's per-frame cinematic update.
void PSP_TexSubImage2D( GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, const void *rgba )
{
	pspTexture_t	*tex = PSP_TexSlot( pspCurrentTexture );

	if( !tex || !tex->used || !rgba || width <= 0 || height <= 0 )
		return;

	if( level < 0 || level > tex->maxLevel || !tex->level[ level ] )
		return;

	// Cinematic images are always 16-bit; a DXT texture here would be a misrouted update.
	if( PSP_TEX_IS_DXT( tex->psm ) )
		return;
#ifdef MISSIONPACK
	if( tex->psm == GU_PSM_T4 )
		return;
#endif

	if( xoffset < 0 || yoffset < 0 ||
	    xoffset + width > tex->w[ level ] || yoffset + height > tex->h[ level ] )
		return;

	PSP_TexBlit( tex, level, xoffset, yoffset, width, height, (const byte *)rgba, width );
	PSP_TexWriteback( tex->level[ level ], PSP_TexLevelBytes( tex->psm, tex->tbw[ level ], tex->h[ level ] ) );
}

#ifdef MISSIONPACK
// qglTexImage2D with a 5650 cinematic frame: a one-level 5650 texture, filled by copy.
void PSP_TexUpload2D5650( GLsizei width, GLsizei height, const void *pixels )
{
	pspTexSrc5650 = qtrue;
	PSP_TexUpload2D( 0, GL_RGB8, width, height, pixels );
	pspTexSrc5650 = qfalse;
}

// qglTexSubImage2D with a 5650 cinematic frame.
void PSP_TexSubImage2D5650( GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, const void *pixels )
{
	pspTexture_t	*tex = PSP_TexSlot( pspCurrentTexture );

	if( !tex || !tex->used || !pixels || width <= 0 || height <= 0 )
		return;

	if( level < 0 || level > tex->maxLevel || !tex->level[ level ] || tex->psm != GU_PSM_5650 )
		return;

	if( xoffset < 0 || yoffset < 0 ||
	    xoffset + width > tex->w[ level ] || yoffset + height > tex->h[ level ] )
		return;

	PSP_TexCopy5650( tex, level, xoffset, yoffset, width, height, (const unsigned short *)pixels );
	PSP_TexWriteback( tex->level[ level ], PSP_TexLevelBytes( tex->psm, tex->tbw[ level ], tex->h[ level ] ) );
}
#endif

// GL keeps filter and wrap per texture, the GE globally, so they are re-emitted at bind.
static void PSP_TexEmitSamplerState( const pspTexture_t *tex )
{
	int	minFilter = tex->minFilter;

	// A mipmap filter on a one-level texture makes the GE sample an undeclared level.
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

// qglTexParameter*: filter and wrap only. GL_TextureMode re-issues these on the bound texture,
// so a change there must be emitted at once or the GE keeps the old state.
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

// qglBindTexture. GL_Bind already drops redundant binds, so this always emits.
// Name 0 is upstream's unbind after creating an image; nothing draws with it, so emit nothing.
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

#ifdef MISSIONPACK
	// 16 entries of 8888 are two 8-entry blocks.
	if( tex->psm == GU_PSM_T4 )
	{
		sceGuClutMode( GU_PSM_8888, 0, 0x0f, 0 );
		sceGuClutLoad( 2, pspTexCluts[ tex->clut ] );
	}
#endif
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

	// The GE texture cache is keyed on address, and a free/malloc pair can reuse one.
	sceGuTexFlush();
}

// Texture memory by format and pool. Non-zero heap means the pool and VRAM ran out.
void PSP_TexMemReport( void )
{
	int	poolUsed = 0, poolTotal = 0, poolLargest = 0;
	int	dxt1 = 0, dxt5 = 0, dxtBytes = 0;
	int	i;
#ifdef MISSIONPACK
	int	t4 = 0, t4Bytes = 0;
#endif

	for( i = 0; i < PSP_MAX_TEXTURES; i++ )
	{
		const pspTexture_t	*tex = &pspTextures[ i ];

#ifdef MISSIONPACK
		if( tex->used && tex->psm == GU_PSM_T4 )
		{
			t4++;
			t4Bytes += tex->bytes;
		}
#endif
		if( !tex->used || !PSP_TEX_IS_DXT( tex->psm ) )
			continue;

		if( tex->psm == GU_PSM_DXT1 )
			dxt1++;
		else
			dxt5++;
		dxtBytes += tex->bytes;
	}

	ri.Printf( PRINT_ALL, "PSP textures: %d images, %d KB, %d swizzled / %d linear; "
		"%d DXT1 + %d DXT5 in %d KB\n",
		pspTexCount,
		pspTexTotalBytes / 1024,
		pspTexSwizzledCount,
		pspTexCount - pspTexSwizzledCount,
		dxt1, dxt5, dxtBytes / 1024 );
#ifdef MISSIONPACK
	ri.Printf( PRINT_ALL, "PSP textures: %d one-colour T4 in %d KB\n", t4, t4Bytes / 1024 );
#endif

	PSP_PoolStats( &poolUsed, &poolTotal, &poolLargest );

	ri.Printf( PRINT_ALL, "PSP texmem: pool %d KB, vram %d KB, heap %d KB "
		"(pool %d/%d KB used, largest free %d KB, vram %d KB free)\n",
		pspTexBytesBySource[ PSP_TEXMEM_POOL ] / 1024,
		pspTexBytesBySource[ PSP_TEXMEM_VRAM ] / 1024,
		pspTexBytesBySource[ PSP_TEXMEM_HEAP ] / 1024,
		poolUsed / 1024, poolTotal / 1024, poolLargest / 1024,
		(int)( vmemavail() / 1024 ) );
}
