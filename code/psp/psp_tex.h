// Texture half of the GL->GU shim: from an RGBA8888 mip level to something the GE samples.
// psp_qgl.c forwards the qgl texture calls here; Upload32 asks PSP_TexChooseFormat for DXT.

#ifndef __PSP_TEX_H__
#define __PSP_TEX_H__

#include "../renderercommon/tr_common.h"

// sceGuTexMode's maxmips is 0-7, so eight levels is the hardware ceiling.
#define PSP_TEX_MAX_LEVELS	8

// Name n lives at index n-1. MAX_DRAWIMAGES is 2048; the slack covers scratch/cinematic images.
#define PSP_MAX_TEXTURES	2080

GLuint	PSP_TexGenName( void );
void	PSP_TexDelete( GLuint name );
void	PSP_TexBind( GLuint name );
void	PSP_TexUpload2D( GLint level, GLenum internalFormat, GLsizei width, GLsizei height, const void *rgba );
void	PSP_TexSubImage2D( GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, const void *rgba );
void	PSP_TexParameter( GLenum pname, GLint value );
void	PSP_TexMemReport( void );
// r_pspDxt 0 keeps every texture 16-bit. r_pspDxtFast 0 encodes with the slower refine pass.
GLenum	PSP_TexChooseFormat( GLenum internalFormat, qboolean alpha, int width, int height,
	qboolean picmip, qboolean mipmap );
#ifdef MISSIONPACK
// Font atlases and glyph art, which R_CreateImage keeps out of DXT.
qboolean	PSP_TexIsTextArt( const char *name );
// Upload32's request for a one-colour alpha texture (GL_ALPHA4's value): a 4-bit CLUT texture.
#define PSP_GL_ONE_COLOUR	0x803B
// TA cinematic frames, decoded by cl_cin.c straight to GE-order 5650.
void	PSP_TexUpload2D5650( GLsizei width, GLsizei height, const void *pixels );
void	PSP_TexSubImage2D5650( GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, const void *pixels );
#endif

#endif // __PSP_TEX_H__
