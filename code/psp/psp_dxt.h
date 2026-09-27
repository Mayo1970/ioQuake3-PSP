#ifndef __PSP_DXT_H__
#define __PSP_DXT_H__

#include "../qcommon/q_shared.h"

// GE DXT1 (8-byte) or DXT5 (16-byte) blocks, row by row; width and height are multiples of 4.
// refine adds the least-squares endpoint pass: better colour, slower load.
void PSP_DxtCompress( qboolean alpha, qboolean refine, const byte *rgba, int width, int height, void *out );

#endif // __PSP_DXT_H__
