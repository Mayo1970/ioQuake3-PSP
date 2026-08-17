/*
===========================================================================
PSP port - code/psp/psp_tcmod.c

Session 12b. Skip ComputeTexCoords' redundant per-stage texture-coordinate
copy by pointing the GE's texcoord array straight at tess.texCoords.

What this removes
-----------------
ComputeTexCoords (renderergl1/tr_shade.c:829) runs per stage, per surface, per
frame. For TCGEN_TEXTURE and TCGEN_LIGHTMAP its entire body is a loop copying
8 bytes per vertex out of tess.texCoords and into tess.svars.texcoords, which
the draw then reads. The copy is pure overhead: the source is already laid out
as vec2_t texCoords[SHADER_MAX_VERTEXES][2] (tr_local.h:1238), i.e. base
coordinates at offset 0 and lightmap coordinates at offset 8 of a 16-byte
stride, and PSP_DrawTexCoordPointer (psp_draw.c:351) already accepts an
arbitrary stride. So the array is repointed instead of refilled.

Every filler was read before relying on this: RB_StretchPic (tr_backend.c:876),
RB_SurfaceMesh (tr_surface.c:761), RB_SurfaceFace and RB_SurfaceGrid all write
tess.texCoords[i][0]. 2D is included - RB_StretchPic fills the same array.

Why not more than this
----------------------
This file originally also folded a stage's affine tcMod chain into the GE's
texture matrix, so that scale/scroll/rotate/transform cost the CPU nothing.
**That was removed after three hardware rounds proved it wrong** (Q3PORT.md
Session 12b): the main menu banner rendered incorrectly with it on, under both
candidate translation columns and with a GU_TEXTURE identity reset in place.

No PSP port in any reference tree - Quake3PSP-mirror, DaedalusX64, ioq3 - uses
GU_TEXTURE_MATRIX at all, so there was never a working example to check the
layout against, and the remaining hypothesis (that GU_TEXTURE_MATRIX + GU_UV
does not work with a 2-float texture coordinate) is not worth another round
against the ranked VFPU work in TODO.md. **Do not re-attempt it without a
known-good PSP example to copy.**

What survives from that attempt, deliberately, because both are correct
regardless: psp_qgl.c now routes GL_TEXTURE to GU_TEXTURE instead of silently
folding it into GU_MODEL (which would have overwritten the view transform),
and excludes it from the clip-plane dirty flag.

Stages this file does NOT claim
-------------------------------
Any stage with a tcMod, and any tcGen the array cannot source directly
(environment, vector, fog, identity). Those take the stock path unchanged, with
nothing touched - which is the only fallback that is safe, since a partially
handled stage would draw from a buffer nobody filled.
===========================================================================
*/

#include "../renderergl1/tr_local.h"
#include "psp_draw.h"

/*
 The array this stage's texture coordinates should be read from, and whether
 this file claimed the stage at all. Set by PSP_TcModFold, consumed by
 PSP_TcModBindArray a few lines later in RB_IterateStagesGeneric.

 Two entry points rather than one because of where the renderer sets the
 pointer. RB_IterateStagesGeneric calls ComputeTexCoords FIRST and only then
 does qglTexCoordPointer( ..., input->svars.texcoords[0] ) - and in the
 single-pass case (setArraysOnce) that call happened even earlier, in
 RB_StageIteratorGeneric. Either way a pointer set from inside ComputeTexCoords
 would be overwritten with svars, which this path deliberately never fills. So
 the decision is made early, to skip the work, and the binding is applied late,
 after the renderer has finished pointing at the buffer being replaced.
*/
static qboolean		pspTcModActive;
static const float	*pspTcModSource;

/*
===============
PSP_TcModFold

Called at the top of ComputeTexCoords. Returns qtrue when it has taken
responsibility for the stage, in which case the caller must return without
doing any per-vertex work.
===============
*/
qboolean PSP_TcModFold( const shaderStage_t *pStage )
{
	const textureBundle_t	*bundle = &pStage->bundle[ 0 ];
	int			tm;

	pspTcModActive = qfalse;

	if( !PSP_DrawFastTexCoords() )
		return qfalse;

	/*
	 Multitexture never happens here - glConfig.numTextureUnits is 1
	 (psp_glimp.c), so CollapseMultitexture never runs and bundle[1] has no
	 image. Checked rather than assumed, because if that ever changes the
	 DrawMultitextured branch does not call PSP_TcModBindArray and this stage
	 would draw from an array nobody filled.
	*/
	if( pStage->bundle[ 1 ].image[ 0 ] != NULL )
		return qfalse;

	/*
	 Only the two tcGens whose source already exists in memory. The rest
	 (environment, vector, fog) are computed per vertex into svars and have no
	 array to point at; TCGEN_IDENTITY is a memset of zeroes and is not worth a
	 code path.
	*/
	if( bundle->tcGen == TCGEN_TEXTURE )
		pspTcModSource = &tess.texCoords[ 0 ][ 0 ][ 0 ];
	else if( bundle->tcGen == TCGEN_LIGHTMAP )
		pspTcModSource = &tess.texCoords[ 0 ][ 1 ][ 0 ];
	else
		return qfalse;

	/*
	 A tcMod rewrites the coordinates, so they can no longer be read from the
	 source array. Those stages keep the stock per-vertex path.

	 This is not the common case it might look like. A plain world surface is a
	 base stage and a lightmap stage and neither carries a tcMod, so most of a
	 frame's stages are claimed here.
	*/
	for( tm = 0; tm < bundle->numTexMods; tm++ )
	{
		if( bundle->texMods[ tm ].type == TMOD_NONE )
			break;			// upstream breaks out of the loop here too

		return qfalse;
	}

	Sys_PSP_RenderProfileCount( PSP_RPROF_FASTTC );
	pspTcModActive = qtrue;

	return qtrue;
}

/*
===============
PSP_TcModBindArray

Called from RB_IterateStagesGeneric immediately before the draw, after the
renderer has set its own texcoord pointer. Points the array at the real
tess.texCoords instead of the svars copy this path never filled.

Does nothing when the stage fell back, which leaves the stock pointer in place.
===============
*/
void PSP_TcModBindArray( void )
{
	if( !pspTcModActive )
		return;

	/*
	 Stride 16, not 0: tess.texCoords is vec2_t[2] per vertex - base coords
	 then lightmap coords - so the two sets interleave at 16 bytes.
	*/
	qglTexCoordPointer( 2, GL_FLOAT, 16, pspTcModSource );

	pspTcModActive = qfalse;
}
