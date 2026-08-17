/*
===========================================================================
PSP port - code/psp/psp_tcmod.h

Session 12b. The two hooks psp_tcmod.c installs into
renderergl1/tr_shade.c, declared here rather than in tr_local.h so the
renderer keeps only the #include and the two call sites.

INCLUDE ORDER MATTERS: shaderStage_t is a typedef of an untagged struct, so
it cannot be forward declared. This header must come after tr_local.h.
===========================================================================
*/

#ifndef __PSP_TCMOD_H__
#define __PSP_TCMOD_H__

/*
 Called at the top of ComputeTexCoords. qtrue means the stage's whole tcGen +
 tcMod chain has been folded into a GE texture matrix and the caller must
 return without doing any per-vertex work. qfalse means nothing was touched
 and the stock path applies.
*/
qboolean	PSP_TcModFold( const shaderStage_t *pStage );

/*
 Called from RB_IterateStagesGeneric just before the draw, after the renderer
 has set its own texcoord pointer. Repoints the array at tess.texCoords and
 arms the GE texture-matrix mode for exactly one draw. No-op for a stage that
 fell back.
*/
void		PSP_TcModBindArray( void );

#endif // __PSP_TCMOD_H__
