/*
===========================================================================
PSP port

Minimal GL 1.1 type/enum compatibility header. code/renderercommon/qgl.h
normally pulls these from <SDL_opengl.h>, but SDL is not present on PSP
(code/sdl/ was pruned down to sdl_glimp.c, kept only as reference). This
header supplies just enough of the desktop-GL vocabulary for qgl.h's
QGL_*_PROCS macros and the ~68 qgl* entry points renderergl1 /
renderercommon reference to typecheck against no-op stubs in psp_glimp.c.

Values are the standard OpenGL 1.1 / registered-extension token values.
Not a real GL implementation - Session 5 replaces the qgl* bodies with
sceGu calls; this header only needs to make the vtable compile.
===========================================================================
*/

#ifndef __PSP_GL_H__
#define __PSP_GL_H__

#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP *
#endif

typedef unsigned int	GLenum;
typedef unsigned char	GLboolean;
typedef unsigned int	GLbitfield;
typedef void		GLvoid;
typedef signed char	GLbyte;
typedef short		GLshort;
typedef int		GLint;
typedef unsigned char	GLubyte;
typedef unsigned short	GLushort;
typedef unsigned int	GLuint;
typedef int		GLsizei;
typedef float		GLfloat;
typedef float		GLclampf;
typedef double		GLdouble;
typedef double		GLclampd;
typedef char		GLchar;
typedef long		GLintptr;
typedef long		GLsizeiptr;

#define GL_FALSE			0x0
#define GL_TRUE				0x1

#define GL_LINES			0x0001
#define GL_TRIANGLES			0x0004
#define GL_TRIANGLE_STRIP		0x0005
#define GL_TRIANGLE_FAN			0x0006
#define GL_QUADS			0x0007
#define GL_POLYGON			0x0009

#define GL_ZERO				0x0
#define GL_ONE				0x1
#define GL_SRC_COLOR			0x0300
#define GL_ONE_MINUS_SRC_COLOR		0x0301
#define GL_SRC_ALPHA			0x0302
#define GL_ONE_MINUS_SRC_ALPHA		0x0303
#define GL_DST_ALPHA			0x0304
#define GL_ONE_MINUS_DST_ALPHA		0x0305
#define GL_DST_COLOR			0x0306
#define GL_ONE_MINUS_DST_COLOR		0x0307
#define GL_SRC_ALPHA_SATURATE		0x0308

#define GL_FRONT_LEFT			0x0400
#define GL_FRONT_RIGHT			0x0401
#define GL_BACK_LEFT			0x0402
#define GL_BACK_RIGHT			0x0403
#define GL_FRONT			0x0404
#define GL_BACK				0x0405
#define GL_FRONT_AND_BACK		0x0408

#define GL_NEVER			0x0200
#define GL_LESS				0x0201
#define GL_EQUAL			0x0202
#define GL_LEQUAL			0x0203
#define GL_GREATER			0x0204
#define GL_NOTEQUAL			0x0205
#define GL_GEQUAL			0x0206
#define GL_ALWAYS			0x0207

#define GL_KEEP				0x1E00
#define GL_REPLACE			0x1E01
#define GL_INCR				0x1E02
#define GL_DECR				0x1E03

#define GL_CULL_FACE			0x0B44
#define GL_DEPTH_TEST			0x0B71
#define GL_STENCIL_TEST			0x0B90
#define GL_BLEND			0x0BE2
#define GL_SCISSOR_TEST			0x0C11
#define GL_ALPHA_TEST			0x0BC0
#define GL_POLYGON_OFFSET_FILL		0x8037
#define GL_CLIP_PLANE0			0x3000

#define GL_DEPTH_BUFFER_BIT		0x00000100
#define GL_STENCIL_BUFFER_BIT		0x00000400
#define GL_COLOR_BUFFER_BIT		0x00004000

#define GL_TEXTURE_2D			0x0DE1
#define GL_TEXTURE_WRAP_S		0x2802
#define GL_TEXTURE_WRAP_T		0x2803
#define GL_TEXTURE_MAG_FILTER		0x2800
#define GL_TEXTURE_MIN_FILTER		0x2801
#define GL_NEAREST			0x2600
#define GL_LINEAR			0x2601
#define GL_NEAREST_MIPMAP_NEAREST	0x2700
#define GL_LINEAR_MIPMAP_NEAREST	0x2701
#define GL_NEAREST_MIPMAP_LINEAR	0x2702
#define GL_LINEAR_MIPMAP_LINEAR	0x2703
#define GL_CLAMP			0x2900
#define GL_REPEAT			0x2901
#define GL_CLAMP_TO_EDGE		0x812F

#define GL_TEXTURE_ENV			0x2300
#define GL_TEXTURE_ENV_MODE		0x2200
#define GL_MODULATE			0x2100
#define GL_DECAL			0x2101
#define GL_ADD				0x0104

#define GL_DEPTH_COMPONENT		0x1902
#define GL_RGB				0x1907
#define GL_RGBA				0x1908
#define GL_LUMINANCE			0x1909
#define GL_LUMINANCE_ALPHA		0x190A
#define GL_STENCIL_INDEX		0x1901

#define GL_RGB5				0x8050
#define GL_RGB8				0x8051
#define GL_RGBA4			0x8056
#define GL_RGBA8			0x8058
#define GL_LUMINANCE8			0x8040
#define GL_LUMINANCE8_ALPHA8		0x8045
#define GL_RGB4_S3TC			0x83A1 // legacy pre-EXT S3TC token

#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT		0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT		0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT		0x83F3
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT		0x8C4D
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT		0x8C4F
#define GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT	0x8C72

#define GL_SRGB_EXT			0x8C40
#define GL_SRGB8_EXT			0x8C41
#define GL_SRGB_ALPHA_EXT		0x8C42
#define GL_SRGB8_ALPHA8_EXT		0x8C43
#define GL_SLUMINANCE_ALPHA_EXT	0x8C44
#define GL_SLUMINANCE8_ALPHA8_EXT	0x8C45
#define GL_SLUMINANCE_EXT		0x8C46
#define GL_SLUMINANCE8_EXT		0x8C47

#define GL_UNSIGNED_BYTE		0x1401
#define GL_UNSIGNED_INT			0x1405
#define GL_FLOAT			0x1406

#define GL_VERTEX_ARRAY			0x8074
#define GL_COLOR_ARRAY			0x8076
#define GL_TEXTURE_COORD_ARRAY		0x8078

#define GL_MODELVIEW			0x1700
#define GL_PROJECTION			0x1701
/*
 GL_TEXTURE was absent until Session 12b because upstream renderergl1 never
 selects it. psp_tcmod.c does, to reach the GE's own texture matrix - see
 gu_MatrixMode, where it must be spelled out or it folds into GU_MODEL and
 overwrites the view transform.
*/
#define GL_TEXTURE			0x1702

#define GL_FLAT				0x1D00
#define GL_SMOOTH			0x1D01

#define GL_FILL				0x1B02
#define GL_LINE				0x1B01

#define GL_MAX_TEXTURE_SIZE		0x0D33
#define GL_MAX_TEXTURE_UNITS_ARB	0x84E2
#define GL_TEXTURE0_ARB			0x84C0
#define GL_TEXTURE1_ARB			0x84C1

#define GL_TEXTURE_MAX_ANISOTROPY_EXT		0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT	0x84FF

#define GL_VENDOR			0x1F00
#define GL_RENDERER			0x1F01
#define GL_VERSION			0x1F02
#define GL_EXTENSIONS			0x1F03

#define GL_NO_ERROR			0x0
#define GL_INVALID_ENUM			0x0500
#define GL_INVALID_VALUE		0x0501
#define GL_INVALID_OPERATION		0x0502
#define GL_STACK_OVERFLOW		0x0503
#define GL_STACK_UNDERFLOW		0x0504
#define GL_OUT_OF_MEMORY		0x0505

#define GL_PACK_ALIGNMENT		0x0D05
#define GL_COLOR_WRITEMASK		0x0C23
#define GL_NUM_EXTENSIONS		0x821D

#endif // __PSP_GL_H__
