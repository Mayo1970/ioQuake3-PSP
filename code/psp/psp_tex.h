/*
===========================================================================
PSP port - code/psp/psp_tex.h

Session 6. The texture-object half of the GL->GU shim: everything between
"renderergl1 handed us an RGBA8888 mip level" and "the GE can sample it".

code/psp/psp_qgl.c owns the qgl* vtable and forwards the eight texture
entry points here. Nothing above psp_qgl.c knows this file exists, and
renderergl1/ stays byte-identical to upstream ioquake3.

Session 6 scope, decided before implementation (Q3PORT.md Session 6):

  - main RAM only. Free VRAM after the framebuffers measured 1.23 MB on
    hardware, because sceGeEdramSetSize is an unresolved user-mode import
    on this firmware (Session 5). Which textures earn VRAM is a Session 11
    decision made against a measured framerate, not a guess made here.
  - 16-bit formats only. GU_PSM_5650 when upstream says the image has no
    alpha, GU_PSM_4444 when it does. No CLUT - a 4-bit palette means
    runtime colour quantisation and a global-CLUT reload per bind, and it
    belongs in the Session 10 asset pipeline where it can be done offline.
===========================================================================
*/

#ifndef __PSP_TEX_H__
#define __PSP_TEX_H__

#include "../renderercommon/tr_common.h"

/*
 sceGuTexMode's maxmips argument has range 0-7, so eight levels is the
 hardware ceiling regardless of what upstream uploads (its chain runs down
 to 1x1, which is ten levels for a 512x512 image).
*/
#define PSP_TEX_MAX_LEVELS	8

/*
 One slot per GL texture name; name n lives at index n-1, and 0 is never
 issued because GL reserves it. renderergl1/tr_local.h:785 caps the engine
 at MAX_DRAWIMAGES 2048; the slack covers the scratch/cinematic images
 created outside that pool.
*/
#define PSP_MAX_TEXTURES	2080

GLuint	PSP_TexGenName( void );
void	PSP_TexDelete( GLuint name );
void	PSP_TexBind( GLuint name );
void	PSP_TexUpload2D( GLint level, GLenum internalFormat, GLsizei width, GLsizei height, const void *rgba );
void	PSP_TexSubImage2D( GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, const void *rgba );
void	PSP_TexParameter( GLenum pname, GLint value );
void	PSP_TexMemReport( void );

#endif // __PSP_TEX_H__
