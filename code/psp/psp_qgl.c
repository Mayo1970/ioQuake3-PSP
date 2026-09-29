// qgl* vtable: GL 1.1 fixed-function calls turned into sceGu commands on psp_glimp.c's open list.
// renderergl1 stays upstream; no-ops: qglClipPlane, qglCopyTexSubImage2D (mirrors show the world behind).

#include "../renderercommon/tr_common.h"
#include "psp_gu.h"
#include "psp_tex.h"
#include "psp_draw.h"

// Translation helpers

// GU compare enums are not GL values shifted (GU_LESS 4, GU_ALWAYS 1), so map each one.
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

// The GE has no ZERO/ONE factor: use GU_FIX with fix 0 / 0xFFFFFF (format-independent, unlike the
// mirror's DST_ALPHA trick). GU_OTHER_COLOR covers GL_DST_COLOR as src and GL_SRC_COLOR as dst.
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

		// No saturate on the GE; source alpha is closest, and stock shaders never use it.
		case GL_SRC_ALPHA_SATURATE:	return GU_SRC_ALPHA;

		default:
			*fix = 0x00FFFFFF;
			return GU_FIX;
	}
}

// sceGumLoadMatrix uses lv.q, which faults on a misaligned address; ioq3's float[16] fields have
// no alignment, so stage them through this aligned copy.
static ScePspFMatrix4 __attribute__((aligned(16))) pspMatrixStage;

static void PSP_LoadMatrix( const GLfloat *m )
{
	Com_Memcpy( &pspMatrixStage, m, sizeof( pspMatrixStage ) );
	sceGumLoadMatrix( &pspMatrixStage );
	sceGumUpdateMatrix();
}

// Current GL_MATRIX_MODE stack; PSP_UpdateClipPlanes reads it to restore the mode (libpspgum can't).
int	pspMatrixModeCurrent = GU_MODEL;

// sceGu* emits commands even for unchanged values, so only real transitions go out. Display-list
// state: reset whenever the GU is reinitialised.
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

// QGL_1_1_PROCS

// Textures: bodies and the GL-vs-GE reasoning live in psp_tex.c.

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

// Upstream only uploads GL_RGBA bytes; internalFormat is its "uses alpha" verdict.
// TA cinematics (RE_UploadCinematic) are the one exception: GE-order 5650 frames.
static void APIENTRY gu_TexImage2D( GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels )
{
#ifdef MISSIONPACK
	if( format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5 )
	{
		PSP_TexUpload2D5650( width, height, pixels );
		return;
	}
#endif
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
#ifdef MISSIONPACK
	if( format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5 )
	{
		PSP_TexSubImage2D5650( level, xoffset, yoffset, width, height, pixels );
		return;
	}
#endif
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

// Framebuffer-to-texture copy (mirror/portal capture), not reached by this port.
static void APIENTRY stub_CopyTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height ) { }

// Geometry: bodies in psp_draw.c.

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

// The list stays open from GLimp_Init to GLimp_Shutdown; syncing here would close it early.
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

// 0xAABBGGRR, and a set bit BLOCKS the write (inverse of GL). Mask all 8 bits even on 5650.
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

// The GE only culls back faces; culling GL_FRONT means declaring the opposite winding as front.
// GU_CCW is the resting state, as in GL.
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

// GL's scissor origin is bottom-left, the GE's top-left: flip Y only. The mirror skips the flip,
// which breaks portal sub-rectangles.
static void APIENTRY gu_Scissor( GLint x, GLint y, GLsizei width, GLsizei height )
{
	if( !pspGuReady )
		return;

	sceGuScissor( x, glConfig.vidHeight - ( y + height ), width, height );
}

// sceGuViewport takes the viewport CENTRE in the 4096x4096 space (panel at 2048 - w/2, 2048 - h/2),
// Y flipped from GL. The GE viewport already negates Y, so geometry needs no extra flip.
static void APIENTRY gu_Viewport( GLint x, GLint y, GLsizei width, GLsizei height )
{
	if( !pspGuReady )
		return;

	sceGuViewport( 2048 - ( glConfig.vidWidth  / 2 ) + x + ( width  / 2 ),
	               2048 + ( glConfig.vidHeight / 2 ) - y - ( height / 2 ),
	               width, height );
}

// QGL_1_1_FIXED_FUNCTION_PROCS

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

// LoadMatrixf into PROJECTION is RB_BeginDrawingView leaving 2D (gu_Ortho enters it);
// a GL_MODELVIEW load only dirties the cached clip planes.
static void APIENTRY gu_LoadMatrixf( const GLfloat *m )
{
	if( !pspGuReady || !m )
		return;

	PSP_LoadMatrix( m );

	if( pspMatrixModeCurrent == GU_PROJECTION )
		PSP_DrawSetOrtho( qfalse );
	else if( pspMatrixModeCurrent != GU_TEXTURE )
		PSP_DrawMatrixDirty();

	// GU_TEXTURE must not dirty the clip planes: psp_tcmod.c loads one per stage, and a recompute
	// each time costs more than the texture matrix saves.
}

// GL has one modelview; GLimp_Init pins GU_VIEW to identity, so it goes to GU_MODEL. GL_TEXTURE is
// explicit, or tcMod matrices would overwrite the model transform.
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

// Only GL_TexEnv calls this. GU_TCC_RGBA keeps texture alpha for the alpha test.
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

// QGL_DESKTOP_1_1_PROCS

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

// Straight scale onto GLimp_Init's 0..65535 LEQUAL range, so DepthRange(0, 0) stays the near plane.
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

// Immediate mode (bodies in psp_draw.c, one sceGuDrawArray at qglEnd); DrawSkySide draws the
// whole skybox with it.
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

// No user clip plane on the GE, and the eye-space plane can't be synthesised cheaply: mirrors show
// the world behind them, as in Quake3PSP-mirror.
static void APIENTRY stub_ClipPlane( GLenum plane, const GLdouble *equation ) { }

// These set the current colour the array path uses without GL_COLOR_ARRAY (sky, debug paths).
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

// RE_StretchRaw and RB_ShowImages draw through the 3D pipe, so z = 0 as GL's glVertex2f means.
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

// RB_SetGL2D always loads identity first, so sceGumOrtho's multiply matches GL (top-left origin).
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

// QGL_3_0_PROCS
static const GLubyte * APIENTRY stub_GetStringi( GLenum name, GLuint index )
{
	return (const GLubyte *)"";
}

// The vtable tr_local.h externs, via qgl.h's QGL_*_PROCS lists.

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

// NULL: one texture unit makes tr_init.c take the no-multitexture path; no CVA to emulate.
void (APIENTRYP qglActiveTextureARB) (GLenum texture) = NULL;
void (APIENTRYP qglClientActiveTextureARB) (GLenum texture) = NULL;
void (APIENTRYP qglMultiTexCoord2fARB) (GLenum target, GLfloat s, GLfloat t) = NULL;

void (APIENTRYP qglLockArraysEXT) (GLint first, GLsizei count) = NULL;
void (APIENTRYP qglUnlockArraysEXT) (void) = NULL;

int qglMajorVersion = 1, qglMinorVersion = 1;
int qglesMajorVersion = 0, qglesMinorVersion = 0;
