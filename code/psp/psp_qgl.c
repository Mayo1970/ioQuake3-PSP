/*
===========================================================================
PSP port - code/psp/psp_qgl.c

The qgl* vtable: GL 1.1 fixed-function calls translated to sceGu commands
appended to the display list psp_glimp.c keeps open.

This is the whole point of the port's renderer architecture. code/renderergl1
is byte-identical to upstream ioquake3 and calls qgl* through function
pointers it already used on every other platform, so nothing above this file
knows the GE exists. The hardware-proven Quake3PSP-mirror took the opposite
route - its renderer/tr_local.h includes pspgu.h and all 364 GL call sites
were rewritten - and it is used here as the semantic oracle for WHICH GU
values to pass, not as a structural template.

Every body is written against a <Name>proc typedef from
code/renderercommon/qgl.h, so a signature drift upstream is a compile error
here rather than a silent function-pointer cast.

SESSION 6 SCOPE. This file is the vtable and the small translations that
fit in one function; anything with real reasoning behind it lives beside
the code it constrains:

  psp_glimp.c  display, VRAM, list, swap                    (Session 5)
  psp_tex.c    texture objects, formats, swizzle, cache     (Session 6)
  psp_draw.c   vertex arrays -> sceGuDrawArray              (Session 6)

Session 7 filled in immediate mode - qglBegin, qglEnd, the qglVertex and
qglTexCoord families and qglArrayElement. The skybox is drawn with it, so
it was never optional. It also gave psp_draw.c's clipper the two facts it
needs from this file: whether the current projection is RB_SetGL2D's ortho,
and whether either matrix has changed since the planes were last extracted.

Still no-ops: the portal clip plane (qglClipPlane) and
framebuffer-to-texture copy (qglCopyTexSubImage2D). The GE has no user clip
plane at all, and neither is needed for a navigable map; the consequence is
that mirror surfaces show the world behind them instead of a reflection,
which is exactly how Quake3PSP-mirror ships. Do not half-implement them
here; a partially wired path renders garbage that is far harder to bisect
than nothing at all.
===========================================================================
*/

#include "../renderercommon/tr_common.h"
#include "psp_gu.h"
#include "psp_tex.h"
#include "psp_draw.h"

/*
=================================================================
Translation helpers
=================================================================
*/

/*
 GL depth/alpha comparison -> GU. The GU enum is not the GL enum: GU_LESS
 is 4 where GL_LESS is 0x0201, and GU_ALWAYS is 1 where GU_NEVER is 0, so
 this cannot be an arithmetic offset.
*/
static int PSP_CompareFunc( GLenum func )
{
	switch( func )
	{
		case GL_NEVER:		return GU_NEVER;
		case GL_LESS:		return GU_LESS;
		case GL_EQUAL:		return GU_EQUAL;
		case GL_LEQUAL:		return GU_LEQUAL;
		case GL_GREATER:	return GU_GREATER;
		case GL_NOTEQUAL:	return GU_NOTEQUAL;
		case GL_GEQUAL:		return GU_GEQUAL;
		case GL_ALWAYS:		return GU_ALWAYS;
		default:		return GU_ALWAYS;
	}
}

/*
 GL blend factor -> GU blend factor, for one operand.

 The GE has no ZERO or ONE factor. GU_FIX supplies a constant instead, and
 sceGuBlendFunc carries a separate fix value per operand, so ZERO is
 GU_FIX/0x000000 and ONE is GU_FIX/0xFFFFFF. The mirror instead maps
 GL_ZERO to GU_DST_ALPHA and GL_ONE to GU_ONE_MINUS_DST_ALPHA
 (renderer/tr_backend.cpp:334-338), which only works because a 5650 target
 has no alpha bits and reads destination alpha as 0. That silently inverts
 the moment the framebuffer becomes 8888 - which Session 11 may well do -
 so use GU_FIX, which is format-independent.

 GU_OTHER_COLOR means "the colour of the other operand": destination colour
 when used as the source factor, source colour when used as the
 destination factor. That is exactly GL_DST_COLOR and GL_SRC_COLOR
 respectively, which is why one enum covers both directions.

 isSource picks which fix slot the caller must fill; *fix receives it.
*/
static int PSP_BlendFactor( GLenum factor, unsigned int *fix )
{
	*fix = 0;

	switch( factor )
	{
		case GL_ZERO:
			*fix = 0x00000000;
			return GU_FIX;
		case GL_ONE:
			*fix = 0x00FFFFFF;
			return GU_FIX;

		// GL_DST_COLOR as a source factor, GL_SRC_COLOR as a dest factor.
		case GL_DST_COLOR:
		case GL_SRC_COLOR:
			return GU_OTHER_COLOR;
		case GL_ONE_MINUS_DST_COLOR:
		case GL_ONE_MINUS_SRC_COLOR:
			return GU_ONE_MINUS_OTHER_COLOR;

		case GL_SRC_ALPHA:		return GU_SRC_ALPHA;
		case GL_ONE_MINUS_SRC_ALPHA:	return GU_ONE_MINUS_SRC_ALPHA;
		case GL_DST_ALPHA:		return GU_DST_ALPHA;
		case GL_ONE_MINUS_DST_ALPHA:	return GU_ONE_MINUS_DST_ALPHA;

		// No saturating factor on the GE. Plain source alpha is the closest
		// behaviour; ioquake3 only reaches this through GLS_SRCBLEND_ALPHA_SATURATE,
		// which stock shaders do not use.
		case GL_SRC_ALPHA_SATURATE:	return GU_SRC_ALPHA;

		default:
			*fix = 0x00FFFFFF;
			return GU_FIX;
	}
}

/*
 sceGumLoadMatrix takes a 16-byte-aligned matrix: the VFPU build of libpspgum
 loads it with lv.q, which raises an exception on a misaligned address rather
 than reading slowly. ioquake3's matrices are bare float[16] fields inside
 structs (backEnd.viewParms.projectionMatrix, backEnd.orientation.modelMatrix)
 with no alignment attribute, so they cannot be cast in place - the mirror
 does exactly that (tr_backend.cpp:517) and is relying on luck in the struct
 layout. Stage through an aligned copy instead; it is 64 bytes.
*/
static ScePspFMatrix4 __attribute__((aligned(16))) pspMatrixStage;

static void PSP_LoadMatrix( const GLfloat *m )
{
	Com_Memcpy( &pspMatrixStage, m, sizeof( pspMatrixStage ) );
	sceGumLoadMatrix( &pspMatrixStage );
	sceGumUpdateMatrix();
}

/*
 Which gum stack GL_MATRIX_MODE currently points at.

 psp_draw.c reads this so PSP_UpdateClipPlanes can borrow both stacks with
 sceGumStoreMatrix and put the mode back exactly as it found it. Tracking it
 here is free - gu_MatrixMode is the only thing that changes it - and asking
 libpspgum for it is not possible.
*/
int	pspMatrixModeCurrent = GU_MODEL;

/*
 The renderer changes the same fixed-function state for nearly every shader
 stage. sceGu* calls append command words even when the value is unchanged,
 so keep the last values in the qgl shim and emit only real transitions.
 This cache is display-list state, not engine state: reset it whenever the GU
 is reinitialised, and leave the first command for every cached value intact.
*/
typedef struct {
	unsigned int	knownCaps;
	unsigned int	enabledCaps;
	qboolean	blendValid;
	GLenum		blendSrc;
	GLenum		blendDst;
	qboolean	colorMaskValid;
	unsigned int	colorMask;
	qboolean	cullValid;
	GLenum		cullMode;
	qboolean	depthFuncValid;
	GLenum		depthFunc;
	qboolean	depthMaskValid;
	GLboolean	depthMask;
	qboolean	alphaValid;
	GLenum		alphaFunc;
	GLclampf	alphaRef;
	qboolean	texEnvValid;
	GLenum		texEnvMode;
	qboolean	shadeValid;
	GLenum		shadeMode;
	qboolean	depthRangeValid;
	int		depthNear;
	int		depthFar;
} pspGuStateCache_t;

static pspGuStateCache_t pspGuState;

void PSP_QGL_ResetStateCache( void )
{
	Com_Memset( &pspGuState, 0, sizeof( pspGuState ) );
	pspMatrixModeCurrent = GU_MODEL;
}

static unsigned int PSP_GLCapBit( GLenum cap )
{
	switch( cap )
	{
		case GL_DEPTH_TEST:	return 1U << 0;
		case GL_CULL_FACE:	return 1U << 1;
		case GL_BLEND:		return 1U << 2;
		case GL_ALPHA_TEST:	return 1U << 3;
		case GL_SCISSOR_TEST:	return 1U << 4;
		case GL_TEXTURE_2D:	return 1U << 5;
		default:		return 0;
	}
}

/*
=================================================================
QGL_1_1_PROCS
=================================================================
*/

/*
=================================================================
Textures. Bodies in code/psp/psp_tex.c.

Every one of these is a straight forward: the GL-vs-GE reasoning
(formats, swizzling, tbw padding, the mip-chain cap, cache writeback)
lives in psp_tex.c so it is stated once, next to the code it constrains.
=================================================================
*/

static void APIENTRY gu_GenTextures( GLsizei n, GLuint *textures )
{
	GLsizei	i;

	for( i = 0; i < n; i++ )
		textures[ i ] = PSP_TexGenName();
}

static void APIENTRY gu_DeleteTextures( GLsizei n, const GLuint *textures )
{
	GLsizei	i;

	for( i = 0; i < n; i++ )
		PSP_TexDelete( textures[ i ] );
}

static void APIENTRY gu_BindTexture( GLenum target, GLuint texture )
{
	PSP_TexBind( texture );
}

/*
 format and type are GL_RGBA / GL_UNSIGNED_BYTE at all four call sites
 (tr_image.c:746,778,800 and tr_backend.c:794) - upstream never uploads
 anything else - and border is always 0. internalFormat is the only
 argument that varies, and it is upstream's own "does this image use
 alpha" verdict; psp_tex.c reads it as such.
*/
static void APIENTRY gu_TexImage2D( GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels )
{
	if( format != GL_RGBA || type != GL_UNSIGNED_BYTE )
	{
		ri.Printf( PRINT_WARNING, "qglTexImage2D: unsupported format 0x%04X type 0x%04X\n",
			(unsigned int)format, (unsigned int)type );
		return;
	}

	PSP_TexUpload2D( level, (GLenum)internalFormat, width, height, pixels );
}

static void APIENTRY gu_TexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels )
{
	if( format != GL_RGBA || type != GL_UNSIGNED_BYTE )
		return;

	PSP_TexSubImage2D( level, xoffset, yoffset, width, height, pixels );
}

static void APIENTRY gu_TexParameterf( GLenum target, GLenum pname, GLfloat param )
{
	PSP_TexParameter( pname, (GLint)param );
}

static void APIENTRY gu_TexParameteri( GLenum target, GLenum pname, GLint param )
{
	PSP_TexParameter( pname, param );
}

/*
 Framebuffer readback into a texture. The only caller is R_MipMap-free
 mirror/portal capture, which this port does not reach yet; sceGuCopyImage
 could implement it, but writing it before something calls it would be
 untestable code. Session 7 owns portals.
*/
static void APIENTRY stub_CopyTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height ) { }

/*
=================================================================
Geometry. Bodies in code/psp/psp_draw.c.
=================================================================
*/

// No caller in renderergl1 - every draw goes through qglDrawElements.
static void APIENTRY stub_DrawArrays( GLenum mode, GLint first, GLsizei count ) { }

static void APIENTRY gu_DrawElements( GLenum mode, GLsizei count, GLenum type, const GLvoid *indices )
{
	PSP_DrawElements( mode, count, type, indices );
}

// --- Not applicable: no stencil bits in a 5650 target. ---
static void APIENTRY stub_ClearStencil( GLint s ) { }
static void APIENTRY stub_StencilFunc( GLenum func, GLint ref, GLuint mask ) { }
static void APIENTRY stub_StencilMask( GLuint mask ) { }
static void APIENTRY stub_StencilOp( GLenum fail, GLenum zfail, GLenum zpass ) { }

// --- No GE equivalent. ---
static void APIENTRY stub_LineWidth( GLfloat width ) { }
static void APIENTRY stub_PolygonOffset( GLfloat factor, GLfloat units ) { }
static void APIENTRY stub_ReadPixels( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels ) { }

/*
 The display list stays open from GLimp_Init to GLimp_Shutdown, so these
 must not sync: sceGuSync would close a list that GLimp_EndFrame is the
 only function allowed to close. r_finish has no meaning here.
*/
static void APIENTRY stub_Finish( void ) { }
static void APIENTRY stub_Flush( void ) { }

static void APIENTRY gu_BlendFunc( GLenum sfactor, GLenum dfactor )
{
	unsigned int	srcFix, dstFix;
	int		src, dst;

	if( !pspGuReady )
		return;

	if( pspGuState.blendValid && pspGuState.blendSrc == sfactor &&
		pspGuState.blendDst == dfactor )
		return;

	pspGuState.blendValid = qtrue;
	pspGuState.blendSrc = sfactor;
	pspGuState.blendDst = dfactor;

	src = PSP_BlendFactor( sfactor, &srcFix );
	dst = PSP_BlendFactor( dfactor, &dstFix );

	sceGuBlendFunc( GU_ADD, src, dst, srcFix, dstFix );
}

static void APIENTRY gu_ClearColor( GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha )
{
	if( !pspGuReady )
		return;

	sceGuClearColor( GU_RGBA( (unsigned int)( red   * 255.0f ),
	                          (unsigned int)( green * 255.0f ),
	                          (unsigned int)( blue  * 255.0f ),
	                          (unsigned int)( alpha * 255.0f ) ) );
}

static void APIENTRY gu_Clear( GLbitfield mask )
{
	int	bits = 0;

	if( !pspGuReady )
		return;

	if( mask & GL_COLOR_BUFFER_BIT )
		bits |= GU_COLOR_BUFFER_BIT;
	if( mask & GL_DEPTH_BUFFER_BIT )
		bits |= GU_DEPTH_BUFFER_BIT;
	if( mask & GL_STENCIL_BUFFER_BIT )
		bits |= GU_STENCIL_BUFFER_BIT;

	if( bits )
		sceGuClear( bits );
}

/*
 0xAABBGGRR, and a 1-bit PREVENTS a write - the inverse of GL's "true means
 writable". The channel masks are 8-bit even though the target is 5650;
 pspgu.h's own note says the unused low bits must be masked too.
*/
static void APIENTRY gu_ColorMask( GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha )
{
	unsigned int	mask = 0;

	if( !pspGuReady )
		return;

	if( !red )		mask |= 0x000000FF;
	if( !green )		mask |= 0x0000FF00;
	if( !blue )		mask |= 0x00FF0000;
	if( !alpha )		mask |= 0xFF000000;

	if( pspGuState.colorMaskValid && pspGuState.colorMask == mask )
		return;

	pspGuState.colorMaskValid = qtrue;
	pspGuState.colorMask = mask;

	sceGuPixelMask( mask );
}

/*
 The GE cannot choose which face to cull; it culls back faces and lets you
 say which winding is front. Culling GL_FRONT is therefore expressed by
 declaring the opposite winding to be the front one.

 GLimp_Init leaves GU_CCW as the resting state, matching GL's default.
*/
static void APIENTRY gu_CullFace( GLenum mode )
{
	if( !pspGuReady )
		return;

	if( pspGuState.cullValid && pspGuState.cullMode == mode )
		return;

	pspGuState.cullValid = qtrue;
	pspGuState.cullMode = mode;

	sceGuFrontFace( ( mode == GL_FRONT ) ? GU_CW : GU_CCW );
}

static void APIENTRY gu_DepthFunc( GLenum func )
{
	if( !pspGuReady )
		return;

	if( pspGuState.depthFuncValid && pspGuState.depthFunc == func )
		return;

	pspGuState.depthFuncValid = qtrue;
	pspGuState.depthFunc = func;

	sceGuDepthFunc( PSP_CompareFunc( func ) );
}

// Inverted: sceGuDepthMask(1) DISABLES depth writes.
static void APIENTRY gu_DepthMask( GLboolean flag )
{
	if( !pspGuReady )
		return;

	if( pspGuState.depthMaskValid && pspGuState.depthMask == flag )
		return;

	pspGuState.depthMaskValid = qtrue;
	pspGuState.depthMask = flag;

	sceGuDepthMask( flag ? GU_FALSE : GU_TRUE );
}

static void APIENTRY gu_Disable( GLenum cap )
{
	unsigned int bit;

	if( !pspGuReady )
		return;

	bit = PSP_GLCapBit( cap );
	if( bit )
	{
		if( ( pspGuState.knownCaps & bit ) && !( pspGuState.enabledCaps & bit ) )
			return;
		pspGuState.knownCaps |= bit;
		pspGuState.enabledCaps &= ~bit;
	}

	switch( cap )
	{
		case GL_DEPTH_TEST:	sceGuDisable( GU_DEPTH_TEST ); break;
		case GL_CULL_FACE:	sceGuDisable( GU_CULL_FACE ); break;
		case GL_BLEND:		sceGuDisable( GU_BLEND ); break;
		case GL_ALPHA_TEST:	sceGuDisable( GU_ALPHA_TEST ); break;
		case GL_SCISSOR_TEST:	sceGuDisable( GU_SCISSOR_TEST ); break;
		case GL_TEXTURE_2D:	sceGuDisable( GU_TEXTURE_2D ); break;

		// GL_POLYGON_OFFSET_FILL: no GE equivalent.
		// GL_CLIP_PLANE0: the portal clip plane, Session 7.
		default:		break;
	}
}

static void APIENTRY gu_Enable( GLenum cap )
{
	unsigned int bit;

	if( !pspGuReady )
		return;

	bit = PSP_GLCapBit( cap );
	if( bit )
	{
		if( ( pspGuState.knownCaps & bit ) && ( pspGuState.enabledCaps & bit ) )
			return;
		pspGuState.knownCaps |= bit;
		pspGuState.enabledCaps |= bit;
	}

	switch( cap )
	{
		case GL_DEPTH_TEST:	sceGuEnable( GU_DEPTH_TEST ); break;
		case GL_CULL_FACE:	sceGuEnable( GU_CULL_FACE ); break;
		case GL_BLEND:		sceGuEnable( GU_BLEND ); break;
		case GL_ALPHA_TEST:	sceGuEnable( GU_ALPHA_TEST ); break;
		case GL_SCISSOR_TEST:	sceGuEnable( GU_SCISSOR_TEST ); break;
		case GL_TEXTURE_2D:	sceGuEnable( GU_TEXTURE_2D ); break;
		default:		break;
	}
}

static void APIENTRY gu_GetBooleanv( GLenum pname, GLboolean *params )
{
	if( params )
		*params = GL_FALSE;
}

static GLenum APIENTRY gu_GetError( void )
{
	return GL_NO_ERROR;
}

static void APIENTRY gu_GetIntegerv( GLenum pname, GLint *params )
{
	if( !params )
		return;

	switch( pname )
	{
		case GL_MAX_TEXTURE_SIZE:	*params = 512; break;
		case GL_MAX_TEXTURE_UNITS_ARB:	*params = 1; break;
		default:			*params = 0; break;
	}
}

static const GLubyte * APIENTRY gu_GetString( GLenum name )
{
	switch( name )
	{
		case GL_VENDOR:		return (const GLubyte *)"Sony";
		case GL_RENDERER:	return (const GLubyte *)"PSPGU";
		case GL_VERSION:	return (const GLubyte *)"sceGu 1.1";
		case GL_EXTENSIONS:	return (const GLubyte *)"";
		default:		return (const GLubyte *)"";
	}
}

/*
 GL's scissor origin is the bottom-left of the framebuffer; the GE's is the
 top-left of the panel. Only the Y needs converting - sceGuScissor really
 does take (x, y, width, height), verified by disassembling libpspgu, so
 unlike the viewport there is no coordinate-space change on top of the flip.

 The mirror omits this flip (tr_backend.cpp:528) and is correct only because
 Quake 3's main scissor covers the whole screen; portal and mirror views set
 a sub-rectangle and would land upside down.
*/
static void APIENTRY gu_Scissor( GLint x, GLint y, GLsizei width, GLsizei height )
{
	if( !pspGuReady )
		return;

	sceGuScissor( x, glConfig.vidHeight - ( y + height ), width, height );
}

/*
 sceGuViewport takes the CENTRE of the viewport in the GE's 4096x4096
 virtual space, not a corner in screen space. GLimp_Init put the panel's
 top-left at (2048 - w/2, 2048 - h/2) with sceGuOffset, so a viewport whose
 top edge is at screen row (vidHeight - (y + height)) has its centre at:

     cx = (2048 - vidWidth/2)  + x + width/2
     cy = (2048 - vidHeight/2) + (vidHeight - (y + height)) + height/2

 which reduces to the expressions below. Full-screen this gives
 (2048, 2048), matching the canonical init.

 No extra vertical flip is needed for the geometry itself: the GE's viewport
 transform already negates Y, which is why an unmodified GL projection
 matrix comes out the right way up.

 The mirror uses cx = 2048 - viewportX (tr_backend.cpp:526-527), which
 agrees with this only when the viewport is the whole screen.
*/
static void APIENTRY gu_Viewport( GLint x, GLint y, GLsizei width, GLsizei height )
{
	if( !pspGuReady )
		return;

	sceGuViewport( 2048 - ( glConfig.vidWidth  / 2 ) + x + ( width  / 2 ),
	               2048 + ( glConfig.vidHeight / 2 ) - y - ( height / 2 ),
	               width, height );
}

/*
=================================================================
QGL_1_1_FIXED_FUNCTION_PROCS
=================================================================
*/

static void APIENTRY gu_Color4f( GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha )
{
	PSP_DrawSetColor4f( red, green, blue, alpha );
}

static void APIENTRY gu_ColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr )
{
	PSP_DrawColorPointer( size, type, stride, ptr );
}

static void APIENTRY gu_DisableClientState( GLenum cap )
{
	PSP_DrawEnableClientState( cap, qfalse );
}

static void APIENTRY gu_EnableClientState( GLenum cap )
{
	PSP_DrawEnableClientState( cap, qtrue );
}

static void APIENTRY gu_TexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr )
{
	PSP_DrawTexCoordPointer( size, type, stride, ptr );
}

static void APIENTRY gu_VertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr )
{
	PSP_DrawVertexPointer( size, type, stride, ptr );
}

static void APIENTRY gu_AlphaFunc( GLenum func, GLclampf ref )
{
	if( !pspGuReady )
		return;

	if( pspGuState.alphaValid && pspGuState.alphaFunc == func &&
		pspGuState.alphaRef == ref )
		return;

	pspGuState.alphaValid = qtrue;
	pspGuState.alphaFunc = func;
	pspGuState.alphaRef = ref;

	sceGuAlphaFunc( PSP_CompareFunc( func ), (int)( ref * 255.0f ), 0xFF );
}

static void APIENTRY gu_LoadIdentity( void )
{
	if( !pspGuReady )
		return;

	sceGumLoadIdentity();
	sceGumUpdateMatrix();

	// Not for GU_TEXTURE - see the note in gu_LoadMatrixf.
	if( pspMatrixModeCurrent != GU_TEXTURE )
		PSP_DrawMatrixDirty();
}

/*
 The one call that tells the clipper it is looking at a 3D frame.

 RB_SetGL2D does MatrixMode(PROJECTION) -> LoadIdentity -> Ortho, so the
 ortho flag goes up in gu_Ortho below. RB_BeginDrawingView does
 MatrixMode(PROJECTION) -> LoadMatrixf(viewParms.projectionMatrix), which is
 the only way back out of 2D. Anything loaded into GL_MODELVIEW just dirties
 the cached planes.
*/
static void APIENTRY gu_LoadMatrixf( const GLfloat *m )
{
	if( !pspGuReady || !m )
		return;

	PSP_LoadMatrix( m );

	if( pspMatrixModeCurrent == GU_PROJECTION )
		PSP_DrawSetOrtho( qfalse );
	else if( pspMatrixModeCurrent != GU_TEXTURE )
		PSP_DrawMatrixDirty();

	/*
	 GU_TEXTURE is excluded deliberately, and it is not a micro-optimisation.
	 The clip planes are extracted from projection x model only
	 (PSP_UpdateClipPlanes), so a texture matrix cannot invalidate them - but
	 psp_tcmod.c loads one PER STAGE, so treating it as a dirtying event would
	 charge every stage of every surface two sceGumStoreMatrix calls, a 4x4
	 multiply and four sqrtf. That is more than the per-vertex work the texture
	 matrix exists to remove, i.e. the change would pay for itself in the wrong
	 direction and look like the GE path simply not helping.

	 If drawOutcode or drawClip ever moves in a run where only tcMod code
	 changed, this branch is the first thing to check.
	*/
}

/*
 GL has one modelview matrix; the GE has separate model and view stacks.
 GLimp_Init pinned GU_VIEW to identity for the life of the process, so
 GL_MODELVIEW lands entirely on GU_MODEL.

 GL_TEXTURE is spelled out (Session 12b) because the old two-way expression
 folded it into GU_MODEL: psp_tcmod.c's first qglLoadMatrixf would have
 overwritten the view transform with a texture matrix, which is a black or
 scrambled frame rather than a subtle bug. Anything else still falls to
 GU_MODEL - GL_MODELVIEW is the only other mode this engine sets.
*/
static void APIENTRY gu_MatrixMode( GLenum mode )
{
	int nextMode;

	if( !pspGuReady )
		return;

	switch( mode )
	{
		case GL_PROJECTION:	nextMode = GU_PROJECTION;	break;
		case GL_TEXTURE:	nextMode = GU_TEXTURE;	break;
		default:		nextMode = GU_MODEL;	break;
	}

	if( pspMatrixModeCurrent == nextMode )
		return;

	pspMatrixModeCurrent = nextMode;
	sceGumMatrixMode( pspMatrixModeCurrent );
}

static void APIENTRY gu_PopMatrix( void )
{
	if( !pspGuReady )
		return;

	sceGumPopMatrix();
	sceGumUpdateMatrix();

	// Not for GU_TEXTURE - see the note in gu_LoadMatrixf.
	if( pspMatrixModeCurrent != GU_TEXTURE )
		PSP_DrawMatrixDirty();
}

static void APIENTRY gu_PushMatrix( void )
{
	if( !pspGuReady )
		return;

	sceGumPushMatrix();
}

static void APIENTRY gu_ShadeModel( GLenum mode )
{
	if( !pspGuReady )
		return;

	if( pspGuState.shadeValid && pspGuState.shadeMode == mode )
		return;

	pspGuState.shadeValid = qtrue;
	pspGuState.shadeMode = mode;

	sceGuShadeModel( ( mode == GL_FLAT ) ? GU_FLAT : GU_SMOOTH );
}

/*
 Only ever called as qglTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode)
 from GL_TexEnv (tr_backend.c). GU_TCC_RGBA keeps the texture's alpha in
 play, which the alpha test above depends on.
*/
static void APIENTRY gu_TexEnvf( GLenum target, GLenum pname, GLfloat param )
{
	if( !pspGuReady || pname != GL_TEXTURE_ENV_MODE )
		return;

	if( pspGuState.texEnvValid && pspGuState.texEnvMode == (GLenum)param )
		return;

	pspGuState.texEnvValid = qtrue;
	pspGuState.texEnvMode = (GLenum)param;

#if PSP_GFX_DEBUG
	{
		static int	dumped = 0;

		if( dumped < 3 )
		{
			dumped++;
			ri.Printf( PRINT_ALL, "TEXENV mode %04X\n", (unsigned int)param );
		}
	}
#endif

	switch( (GLenum)param )
	{
		case GL_MODULATE:	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA ); break;
		case GL_REPLACE:	sceGuTexFunc( GU_TFX_REPLACE,  GU_TCC_RGBA ); break;
		case GL_DECAL:		sceGuTexFunc( GU_TFX_DECAL,    GU_TCC_RGBA ); break;
		case GL_ADD:		sceGuTexFunc( GU_TFX_ADD,      GU_TCC_RGBA ); break;
		default:		break;
	}
}

static void APIENTRY gu_Translatef( GLfloat x, GLfloat y, GLfloat z )
{
	ScePspFVector3 __attribute__((aligned(16)))	v;

	if( !pspGuReady )
		return;

	v.x = x;
	v.y = y;
	v.z = z;

	sceGumTranslate( &v );
	sceGumUpdateMatrix();
	PSP_DrawMatrixDirty();
}

/*
=================================================================
QGL_DESKTOP_1_1_PROCS
=================================================================
*/

// Double buffered through sceGuSwapBuffers only; there is no front-buffer
// rendering to select. tr_cmds.c's r_showimages path is the only caller.
static void APIENTRY stub_DrawBuffer( GLenum mode ) { }

// The GE rasterises filled triangles only - no wireframe mode. r_showtris
// therefore has no effect on this platform.
static void APIENTRY stub_PolygonMode( GLenum face, GLenum mode ) { }

static void APIENTRY gu_ClearDepth( GLclampd depth )
{
	if( !pspGuReady )
		return;

	sceGuClearDepth( (unsigned int)( depth * 65535.0 ) );
}

/*
 Not inverted. GLimp_Init established sceGuDepthRange(0, 65535) with
 GU_LEQUAL, so this is a straight scale of GL's [0,1] range and
 qglDepthRange(0, 0) - ioquake3's "never occluded" for flares and debug
 normals - keeps meaning the near plane.
*/
static void APIENTRY gu_DepthRange( GLclampd near_val, GLclampd far_val )
{
	int nearValue, farValue;

	if( !pspGuReady )
		return;

	nearValue = (int)( near_val * 65535.0 );
	farValue = (int)( far_val * 65535.0 );

	if( pspGuState.depthRangeValid && pspGuState.depthNear == nearValue &&
		pspGuState.depthFar == farValue )
		return;

	pspGuState.depthRangeValid = qtrue;
	pspGuState.depthNear = nearValue;
	pspGuState.depthFar = farValue;

	sceGuDepthRange( nearValue, farValue );
}

/*
=================================================================
QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS

Immediate mode. Bodies in code/psp/psp_draw.c, which accumulates into the
same 24-byte vertex layout the array path uses and issues one
sceGuDrawArray at qglEnd.

This is not a debug-only path: DrawSkySide (renderergl1/tr_sky.c:376-393)
draws the entire skybox through it.
=================================================================
*/
static void APIENTRY gu_ArrayElement( GLint i )
{
	PSP_DrawArrayElement( i );
}

static void APIENTRY gu_Begin( GLenum mode )
{
	PSP_DrawBegin( mode );
}

static void APIENTRY gu_End( void )
{
	PSP_DrawEnd();
}

/*
 The portal/mirror clip plane. The GE has no user clip plane of any kind and
 nothing here can synthesise one cheaply - the plane arrives in eye space
 while psp_draw.c's clipper works in model space. Left as a no-op: mirrors
 render the world behind them instead of a reflection, which is what
 Quake3PSP-mirror ships too.
*/
static void APIENTRY stub_ClipPlane( GLenum plane, const GLdouble *equation ) { }

/*
 These two are grouped with immediate mode by qgl.h, but they set the same
 current colour the vertex-array path reads when GL_COLOR_ARRAY is off -
 which is how the sky (tr_sky.c:800) and the untextured debug paths get
 their colour. So they are real even though qglBegin/qglVertex are not.
*/
static void APIENTRY gu_Color3f( GLfloat red, GLfloat green, GLfloat blue )
{
	PSP_DrawSetColor4f( red, green, blue, 1.0f );
}

static void APIENTRY gu_Color4ubv( const GLubyte *v )
{
	PSP_DrawSetColor4ubv( v );
}

// No caller anywhere in renderergl1 - the projection always arrives through
// qglLoadMatrixf or qglOrtho.
static void APIENTRY stub_Frustum( GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val ) { }

static void APIENTRY gu_TexCoord2f( GLfloat s, GLfloat t )
{
	PSP_DrawImmTexCoord2f( s, t );
}

static void APIENTRY gu_TexCoord2fv( const GLfloat *v )
{
	if( v )
		PSP_DrawImmTexCoord2f( v[ 0 ], v[ 1 ] );
}

// The 2D callers (RE_StretchRaw, RB_ShowImages) draw through the same 3D
// T&L pipe as everything else in this port, so z is a real coordinate and
// 0 is what GL's glVertex2f means by it.
static void APIENTRY gu_Vertex2f( GLfloat x, GLfloat y )
{
	PSP_DrawImmVertex3f( x, y, 0.0f );
}

static void APIENTRY gu_Vertex3f( GLfloat x, GLfloat y, GLfloat z )
{
	PSP_DrawImmVertex3f( x, y, z );
}

static void APIENTRY gu_Vertex3fv( const GLfloat *v )
{
	if( v )
		PSP_DrawImmVertex3f( v[ 0 ], v[ 1 ], v[ 2 ] );
}

/*
 RB_SetGL2D's projection. sceGumOrtho multiplies the current matrix, and
 ioquake3 always issues qglLoadIdentity first, so this matches GL exactly -
 including the flipped top/bottom (0, w, h, 0) that puts 2D origin at the
 top-left.
*/
static void APIENTRY gu_Ortho( GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val )
{
	if( !pspGuReady )
		return;

	sceGumOrtho( (float)left, (float)right, (float)bottom, (float)top,
	             (float)near_val, (float)far_val );
	sceGumUpdateMatrix();

	// RB_SetGL2D is the only caller. The frustum clipper must not run on a
	// 2D pass - there is no frustum - and this is how it finds out.
	PSP_DrawSetOrtho( qtrue );

#if PSP_GFX_DEBUG
	{
		static int	dumped = 0;

		if( dumped < 2 )
		{
			dumped++;

			ri.Printf( PRINT_ALL, "ORTHO l %.0f r %.0f b %.0f t %.0f n %.2f f %.2f\n",
				(float)left, (float)right, (float)bottom, (float)top,
				(float)near_val, (float)far_val );
		}
	}
#endif
}

/*
=================================================================
QGL_3_0_PROCS
=================================================================
*/
static const GLubyte * APIENTRY stub_GetStringi( GLenum name, GLuint index )
{
	return (const GLubyte *)"";
}

/*
=================================================================
The vtable tr_local.h externs, via the QGL_*_PROCS lists in
code/renderercommon/qgl.h.
=================================================================
*/

BindTextureproc               *qglBindTexture               = gu_BindTexture;
BlendFuncproc                 *qglBlendFunc                 = gu_BlendFunc;
ClearColorproc                *qglClearColor                = gu_ClearColor;
Clearproc                     *qglClear                     = gu_Clear;
ClearStencilproc              *qglClearStencil              = stub_ClearStencil;
ColorMaskproc                 *qglColorMask                 = gu_ColorMask;
CopyTexSubImage2Dproc         *qglCopyTexSubImage2D         = stub_CopyTexSubImage2D;
CullFaceproc                  *qglCullFace                  = gu_CullFace;
DeleteTexturesproc            *qglDeleteTextures            = gu_DeleteTextures;
DepthFuncproc                 *qglDepthFunc                 = gu_DepthFunc;
DepthMaskproc                 *qglDepthMask                 = gu_DepthMask;
Disableproc                   *qglDisable                   = gu_Disable;
DrawArraysproc                *qglDrawArrays                = stub_DrawArrays;
DrawElementsproc              *qglDrawElements              = gu_DrawElements;
Enableproc                    *qglEnable                    = gu_Enable;
Finishproc                    *qglFinish                    = stub_Finish;
Flushproc                     *qglFlush                     = stub_Flush;
GenTexturesproc               *qglGenTextures               = gu_GenTextures;
GetBooleanvproc               *qglGetBooleanv               = gu_GetBooleanv;
GetErrorproc                  *qglGetError                  = gu_GetError;
GetIntegervproc               *qglGetIntegerv               = gu_GetIntegerv;
GetStringproc                 *qglGetString                 = gu_GetString;
LineWidthproc                 *qglLineWidth                 = stub_LineWidth;
PolygonOffsetproc             *qglPolygonOffset             = stub_PolygonOffset;
ReadPixelsproc                *qglReadPixels                = stub_ReadPixels;
Scissorproc                   *qglScissor                   = gu_Scissor;
StencilFuncproc               *qglStencilFunc               = stub_StencilFunc;
StencilMaskproc               *qglStencilMask               = stub_StencilMask;
StencilOpproc                 *qglStencilOp                 = stub_StencilOp;
TexImage2Dproc                *qglTexImage2D                = gu_TexImage2D;
TexParameterfproc             *qglTexParameterf             = gu_TexParameterf;
TexParameteriproc             *qglTexParameteri             = gu_TexParameteri;
TexSubImage2Dproc             *qglTexSubImage2D             = gu_TexSubImage2D;
Viewportproc                  *qglViewport                  = gu_Viewport;

AlphaFuncproc                 *qglAlphaFunc                 = gu_AlphaFunc;
Color4fproc                   *qglColor4f                   = gu_Color4f;
ColorPointerproc              *qglColorPointer              = gu_ColorPointer;
DisableClientStateproc        *qglDisableClientState        = gu_DisableClientState;
EnableClientStateproc         *qglEnableClientState         = gu_EnableClientState;
LoadIdentityproc              *qglLoadIdentity              = gu_LoadIdentity;
LoadMatrixfproc               *qglLoadMatrixf               = gu_LoadMatrixf;
MatrixModeproc                *qglMatrixMode                = gu_MatrixMode;
PopMatrixproc                 *qglPopMatrix                 = gu_PopMatrix;
PushMatrixproc                *qglPushMatrix                = gu_PushMatrix;
ShadeModelproc                *qglShadeModel                = gu_ShadeModel;
TexCoordPointerproc           *qglTexCoordPointer           = gu_TexCoordPointer;
TexEnvfproc                   *qglTexEnvf                   = gu_TexEnvf;
Translatefproc                *qglTranslatef                = gu_Translatef;
VertexPointerproc             *qglVertexPointer             = gu_VertexPointer;

ClearDepthproc                *qglClearDepth                = gu_ClearDepth;
DepthRangeproc                *qglDepthRange                = gu_DepthRange;
DrawBufferproc                *qglDrawBuffer                = stub_DrawBuffer;
PolygonModeproc               *qglPolygonMode               = stub_PolygonMode;

ArrayElementproc              *qglArrayElement              = gu_ArrayElement;
Beginproc                     *qglBegin                     = gu_Begin;
ClipPlaneproc                 *qglClipPlane                 = stub_ClipPlane;
Color3fproc                   *qglColor3f                   = gu_Color3f;
Color4ubvproc                 *qglColor4ubv                 = gu_Color4ubv;
Endproc                       *qglEnd                       = gu_End;
Frustumproc                   *qglFrustum                   = stub_Frustum;
Orthoproc                     *qglOrtho                     = gu_Ortho;
TexCoord2fproc                *qglTexCoord2f                = gu_TexCoord2f;
TexCoord2fvproc               *qglTexCoord2fv               = gu_TexCoord2fv;
Vertex2fproc                  *qglVertex2f                  = gu_Vertex2f;
Vertex3fproc                  *qglVertex3f                  = gu_Vertex3f;
Vertex3fvproc                 *qglVertex3fv                 = gu_Vertex3fv;

GetStringiproc                *qglGetStringi                = stub_GetStringi;

/*
 Declared directly in qgl.h rather than through a GLE list. They stay NULL:
 the GE has one texture unit, so tr_init.c takes its no-multitexture path
 and never dereferences these, and there is no compiled-vertex-array
 extension to emulate.
*/
void (APIENTRYP qglActiveTextureARB) (GLenum texture) = NULL;
void (APIENTRYP qglClientActiveTextureARB) (GLenum texture) = NULL;
void (APIENTRYP qglMultiTexCoord2fARB) (GLenum target, GLfloat s, GLfloat t) = NULL;

void (APIENTRYP qglLockArraysEXT) (GLint first, GLsizei count) = NULL;
void (APIENTRYP qglUnlockArraysEXT) (void) = NULL;

int qglMajorVersion = 1, qglMinorVersion = 1;
int qglesMajorVersion = 0, qglesMinorVersion = 0;
