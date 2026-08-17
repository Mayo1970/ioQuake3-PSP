/*
===========================================================================
PSP port - code/psp/psp_draw.h

Session 6. The geometry half of the GL->GU shim: GL vertex arrays in,
sceGuDrawArray out.

The Session 6 checklist in Q3PORT.md does not name this file, but its gate
cannot be reached without it. 2D in ioquake3 is not a separate path:
RB_StretchPic fills tess, and RB_EndSurface -> RB_StageIteratorGeneric ->
R_DrawElements (renderergl1/tr_shade.c:168-190) drives exactly the same
qglVertexPointer / qglTexCoordPointer / qglColorPointer / qglDrawElements
quartet the world geometry uses. So console text, the loading screen and
the menu all arrive through here.

Session 7 added the two things that gate a world frame: immediate mode
(the SKYBOX is drawn with it - renderergl1/tr_sky.c:376-393) and the CPU
frustum clipper the GE needs.
===========================================================================
*/

#ifndef __PSP_DRAW_H__
#define __PSP_DRAW_H__

#include "../renderercommon/tr_common.h"

void	PSP_DrawBeginFrame( void );
void	PSP_DrawRegisterCvars( void );
void	PSP_DrawInvalidateBatchCache( void );

// Session 10 - the cached vertex arena that replaced sceGuGetMemory. Paired
// with the display-list allocation in GLimp_Init / GLimp_Shutdown.
qboolean PSP_DrawInitArena( void );
void	PSP_DrawShutdownArena( void );
// PSP_DrawArenaPeakKB is declared in psp_platform.h: sys_psp.c reports it and
// must not pull in tr_common.h to do so.

void	PSP_DrawEnableClientState( GLenum cap, qboolean enable );
void	PSP_DrawVertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr );
void	PSP_DrawTexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr );
void	PSP_DrawColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr );

void	PSP_DrawSetColor4f( GLfloat r, GLfloat g, GLfloat b, GLfloat a );
void	PSP_DrawSetColor4ubv( const GLubyte *v );

void	PSP_DrawElements( GLenum mode, GLsizei count, GLenum type, const GLvoid *indices );

/* Static-world faces are already in the GE vertex layout. */
qboolean PSP_DrawStaticWorldAppend( const void *vertices, int vertexCount,
	const unsigned short *indices, int indexCount );
qboolean PSP_DrawStaticWorldSubmit( void );
void PSP_DrawStaticWorldEnd( void );

// Session 7 - immediate mode.
void	PSP_DrawBegin( GLenum mode );
void	PSP_DrawImmTexCoord2f( GLfloat s, GLfloat t );
void	PSP_DrawImmVertex3f( GLfloat x, GLfloat y, GLfloat z );
void	PSP_DrawArrayElement( GLint i );
void	PSP_DrawEnd( void );

// Session 7 - what the clipper needs to know from the matrix entry points.
void	PSP_DrawSetOrtho( qboolean ortho );
void	PSP_DrawMatrixDirty( void );

/*
 Session 12b - r_pspFastTexCoords, read by psp_tcmod.c. When on, stages that
 need no per-vertex texture-coordinate work read tess.texCoords directly
 instead of a per-stage copy of it. Registered in PSP_DrawRegisterCvars beside
 r_pspClip so every PSP-only renderer cvar lives in one place.
*/
qboolean PSP_DrawFastTexCoords( void );

void	PSP_DrawMemReport( void );

#endif // __PSP_DRAW_H__
