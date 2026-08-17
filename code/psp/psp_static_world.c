/*
===========================================================================
PSP port - code/psp/psp_static_world.c

Static-world cache support. The fixed table retains ARM 1 classification, and
eligible faces are packed lazily into hunk memory for the live draw path.
===========================================================================
*/

#include "../renderergl1/tr_local.h"
#include "psp_draw.h"
#include "psp_static_world.h"

#include <stdint.h>

#define PSP_STATIC_WORLD_TABLE_BITS	12
#define PSP_STATIC_WORLD_TABLE_SIZE	( 1U << PSP_STATIC_WORLD_TABLE_BITS )
#define PSP_STATIC_WORLD_TABLE_MASK	( PSP_STATIC_WORLD_TABLE_SIZE - 1U )
#define PSP_STATIC_WORLD_CACHE_CAP	( 1U * 1024U * 1024U )
#define PSP_STATIC_WORLD_HUNK_MARGIN	( 1U * 1024U * 1024U )

typedef enum {
	PSP_STATIC_WORLD_REJECT_NO_STAGES,
	PSP_STATIC_WORLD_REJECT_MULTISTAGE,
	PSP_STATIC_WORLD_REJECT_DEFORM,
	PSP_STATIC_WORLD_REJECT_TCGEN,
	PSP_STATIC_WORLD_REJECT_TCMOD,
	PSP_STATIC_WORLD_REJECT_MULTITEXTURE,
	PSP_STATIC_WORLD_REJECT_RGBGEN,
	PSP_STATIC_WORLD_REJECT_ALPHAGEN,
	PSP_STATIC_WORLD_REJECT_FOG,
	PSP_STATIC_WORLD_REJECT_DLIGHT,
	PSP_STATIC_WORLD_REJECT_SKY,
	PSP_STATIC_WORLD_REJECT_PORTAL,
	PSP_STATIC_WORLD_REJECT_GEOMETRY,
	PSP_STATIC_WORLD_REJECT_COUNT
} pspStaticWorldReject_t;

typedef struct {
	const srfSurfaceFace_t	*surface;
	qboolean			qualifies;
	pspStaticWorldReject_t	rejectReason;
	unsigned int		cacheBytes;
	void				*cacheBlock;
	float			bounds[ 2 ][ 3 ];
	qboolean			boundsValid;
} pspStaticWorldEntry_t;

typedef struct {
	float			u, v;
	unsigned int	rgba;
	float			x, y, z;
} pspStaticWorldVertex_t;

static pspStaticWorldEntry_t
	pspStaticWorldTable[ PSP_STATIC_WORLD_TABLE_SIZE ];

static unsigned int	pspStaticWorldUnique;
static unsigned int	pspStaticWorldQualifying;
static unsigned int	pspStaticWorldCacheBytes;
static unsigned int	pspStaticWorldDrawCalls;
static unsigned int	pspStaticWorldQualifyingDrawCalls;
static unsigned int	pspStaticWorldOverflowDrawCalls;
static unsigned int	pspStaticWorldRejects[ PSP_STATIC_WORLD_REJECT_COUNT ];
static unsigned int	pspStaticWorldCacheInUse;
static qboolean		pspStaticWorldCacheFull;
static unsigned int	pspStaticWorldCachedDraws;
static unsigned int	pspStaticWorldCachedFaces;
static unsigned int	pspStaticWorldFallthroughNotEligible;
static unsigned int	pspStaticWorldFallthroughBmodel;
static unsigned int	pspStaticWorldFallthroughFrustum;
static unsigned int	pspStaticWorldFallthroughCacheFull;
static qboolean		pspStaticWorldOverflow;
static cvar_t		*r_pspStaticWorld;
static shader_t		*pspStaticWorldRunShader;

static const char * const pspStaticWorldRejectName[ PSP_STATIC_WORLD_REJECT_COUNT ] = {
	"noStages",
	"multiStage",
	"deformVertexes",
	"tcGen",
	"tcMod",
	"multitexture",
	"rgbGen",
	"alphaGen",
	"fog",
	"dlight",
	"sky",
	"portal",
	"geometry"
};

static unsigned int PSP_StaticWorldAlign16( unsigned int bytes )
{
	return ( bytes + 15U ) & ~15U;
}

static unsigned int PSP_StaticWorldHash( const srfSurfaceFace_t *surface )
{
	unsigned int value;

	value = (unsigned int)(uintptr_t)surface;
	value ^= value >> 16;
	value *= 0x7feb352dU;
	value ^= value >> 15;
	value *= 0x846ca68bU;
	value ^= value >> 16;

	return value & PSP_STATIC_WORLD_TABLE_MASK;
}

static pspStaticWorldEntry_t *PSP_StaticWorldFindEntry(
	const srfSurfaceFace_t *surface, qboolean *found )
{
	unsigned int		index;
	unsigned int		probe;
	pspStaticWorldEntry_t	*entry;

	index = PSP_StaticWorldHash( surface );
	*found = qfalse;

	for( probe = 0; probe < PSP_STATIC_WORLD_TABLE_SIZE; probe++ ) {
		entry = &pspStaticWorldTable[ index ];

		if( !entry->surface ) {
			return entry;
		}

		if( entry->surface == surface ) {
			*found = qtrue;
			return entry;
		}

		index = ( index + 1U ) & PSP_STATIC_WORLD_TABLE_MASK;
	}

	return NULL;
}

static void PSP_StaticWorldEnsureBounds( pspStaticWorldEntry_t *entry,
	const srfSurfaceFace_t *surface )
{
	const float *point;
	int i;

	if( entry->boundsValid )
		return;

	point = surface->points[ 0 ];
	VectorCopy( point, entry->bounds[ 0 ] );
	VectorCopy( point, entry->bounds[ 1 ] );
	for( i = 1; i < surface->numPoints; i++ ) {
		point = surface->points[ i ];
		if( point[ 0 ] < entry->bounds[ 0 ][ 0 ] )
			entry->bounds[ 0 ][ 0 ] = point[ 0 ];
		if( point[ 1 ] < entry->bounds[ 0 ][ 1 ] )
			entry->bounds[ 0 ][ 1 ] = point[ 1 ];
		if( point[ 2 ] < entry->bounds[ 0 ][ 2 ] )
			entry->bounds[ 0 ][ 2 ] = point[ 2 ];
		if( point[ 0 ] > entry->bounds[ 1 ][ 0 ] )
			entry->bounds[ 1 ][ 0 ] = point[ 0 ];
		if( point[ 1 ] > entry->bounds[ 1 ][ 1 ] )
			entry->bounds[ 1 ][ 1 ] = point[ 1 ];
		if( point[ 2 ] > entry->bounds[ 1 ][ 2 ] )
			entry->bounds[ 1 ][ 2 ] = point[ 2 ];
	}

	entry->boundsValid = qtrue;
}

static qboolean PSP_StaticWorldInsideFrustum(
	const pspStaticWorldEntry_t *entry )
{
	const cplane_t	*plane;
	vec3_t			positive;
	vec3_t			negative;
	float				dot;
	int				i;
	int				j;

	for( i = 0; i < 4; i++ ) {
		plane = &backEnd.viewParms.frustum[ i ];
		for( j = 0; j < 3; j++ ) {
			if( plane->normal[ j ] >= 0.0f ) {
				positive[ j ] = entry->bounds[ 1 ][ j ];
				negative[ j ] = entry->bounds[ 0 ][ j ];
			} else {
				positive[ j ] = entry->bounds[ 0 ][ j ];
				negative[ j ] = entry->bounds[ 1 ][ j ];
			}
		}

		/* A most-positive vertex behind the plane means the whole box is out. */
		dot = DotProduct( positive, plane->normal );
		if( dot < plane->dist )
			return qfalse;

		/* A most-negative vertex behind the plane means the box straddles it. */
		dot = DotProduct( negative, plane->normal );
		if( dot < plane->dist )
			return qfalse;
	}

	return qtrue;
}

static unsigned int PSP_StaticWorldPackColor( const shaderStage_t *stage,
	const float *point )
{
	unsigned int source;
	unsigned int color;
	unsigned char r;
	unsigned char g;
	unsigned char b;
	unsigned char a;

	source = *(const unsigned int *)&point[ 7 ];
	r = (unsigned char)( source & 0xffU );
	g = (unsigned char)( ( source >> 8 ) & 0xffU );
	b = (unsigned char)( ( source >> 16 ) & 0xffU );
	a = (unsigned char)( ( source >> 24 ) & 0xffU );

	switch( stage->rgbGen ) {
	case CGEN_IDENTITY:
		color = 0xffffffffU;
		break;
	case CGEN_IDENTITY_LIGHTING:
		color = (unsigned int)tr.identityLightByte |
			( (unsigned int)tr.identityLightByte << 8 ) |
			( (unsigned int)tr.identityLightByte << 16 ) |
			( (unsigned int)tr.identityLightByte << 24 );
		break;
	case CGEN_VERTEX:
		if( tr.identityLight == 1.0f ) {
			color = source;
		} else {
			r = (unsigned char)( r * tr.identityLight );
			g = (unsigned char)( g * tr.identityLight );
			b = (unsigned char)( b * tr.identityLight );
			color = (unsigned int)r | ( (unsigned int)g << 8 ) |
				( (unsigned int)b << 16 ) | ( (unsigned int)a << 24 );
		}
		break;
	case CGEN_EXACT_VERTEX:
		color = source;
		break;
	default:
		color = source;
		break;
	}

	if( stage->alphaGen == AGEN_IDENTITY &&
		( stage->rgbGen != CGEN_IDENTITY ) &&
		( stage->rgbGen != CGEN_VERTEX || tr.identityLight != 1.0f ) )
		color = ( color & 0x00ffffffU ) | 0xff000000U;

	return color;
}

static qboolean PSP_StaticWorldPackCache( pspStaticWorldEntry_t *entry,
	const srfSurfaceFace_t *surface, const shader_t *shader )
{
	pspStaticWorldVertex_t	*vertices;
	unsigned short			*indices;
	const shaderStage_t		*stage;
	const float				*point;
	const unsigned			*sourceIndices;
	unsigned int			vertexBytes;
	unsigned int			i;

	if( entry->cacheBlock )
		return qtrue;

	if( pspStaticWorldCacheFull ||
		entry->cacheBytes > PSP_STATIC_WORLD_CACHE_CAP -
			pspStaticWorldCacheInUse ||
		Hunk_MemoryRemaining() < (int)( entry->cacheBytes +
			PSP_STATIC_WORLD_HUNK_MARGIN ) ) {
		pspStaticWorldCacheFull = qtrue;
		return qfalse;
	}

	vertexBytes = PSP_StaticWorldAlign16(
		(unsigned int)surface->numPoints * 24U );
	entry->cacheBlock = ri.Hunk_Alloc( entry->cacheBytes, h_low );
	if( !entry->cacheBlock ) {
		pspStaticWorldCacheFull = qtrue;
		return qfalse;
	}

	vertices = (pspStaticWorldVertex_t *)entry->cacheBlock;
	indices = (unsigned short *)( (byte *)entry->cacheBlock + vertexBytes );
	stage = shader->stages[ 0 ];
	for( i = 0; i < (unsigned int)surface->numPoints; i++ ) {
		point = surface->points[ i ];
		if( stage->bundle[ 0 ].tcGen == TCGEN_LIGHTMAP ) {
			vertices[ i ].u = point[ 5 ];
			vertices[ i ].v = point[ 6 ];
		} else {
			vertices[ i ].u = point[ 3 ];
			vertices[ i ].v = point[ 4 ];
		}
		vertices[ i ].rgba = PSP_StaticWorldPackColor( stage, point );
		vertices[ i ].x = point[ 0 ];
		vertices[ i ].y = point[ 1 ];
		vertices[ i ].z = point[ 2 ];
	}

	sourceIndices = (const unsigned *)( (const byte *)surface +
		surface->ofsIndices );
	for( i = 0; i < (unsigned int)surface->numIndices; i++ )
		indices[ i ] = (unsigned short)sourceIndices[ i ];

	pspStaticWorldCacheInUse += entry->cacheBytes;
	return qtrue;
}

static pspStaticWorldReject_t PSP_StaticWorldCheck(
	const srfSurfaceFace_t *surface, const shader_t *shader,
	int fogNum, unsigned int *cacheBytes, unsigned int *rejectMask )
{
	const shaderStage_t	*stage;
	const textureBundle_t	*bundle;
	unsigned int		vertexBytes;
	unsigned int		indexBytes;
	unsigned int		failedReasons;
	int			stageIndex;
	int			tm;
	int			i;

	/* Count every failure; a first-failure histogram hid a spec bug for a whole
	 * hardware round. */
	failedReasons = 0;
	*cacheBytes = 0;

	if( shader->numDeforms != 0 ) {
		failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_DEFORM;
	}

	if( shader->numUnfoggedPasses <= 0 ||
		shader->numUnfoggedPasses > MAX_SHADER_STAGES ) {
		failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_NO_STAGES;
	}

	if( shader->numUnfoggedPasses != 1 ) {
		failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_MULTISTAGE;
	}

	if( shader->numUnfoggedPasses > 0 &&
		shader->numUnfoggedPasses <= MAX_SHADER_STAGES ) {
		for( stageIndex = 0; stageIndex < shader->numUnfoggedPasses; stageIndex++ ) {
			stage = shader->stages[ stageIndex ];
			if( !stage ) {
				failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_NO_STAGES;
				continue;
			}

			bundle = &stage->bundle[ 0 ];
			if( bundle->tcGen != TCGEN_TEXTURE &&
				bundle->tcGen != TCGEN_LIGHTMAP ) {
				failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_TCGEN;
			}

			if( bundle->numTexMods < 0 ||
				bundle->numTexMods > TR_MAX_TEXMODS ||
				( bundle->numTexMods && !bundle->texMods ) ) {
				failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_TCMOD;
			} else {
				for( tm = 0; tm < bundle->numTexMods; tm++ ) {
					if( bundle->texMods[ tm ].type != TMOD_NONE ) {
						failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_TCMOD;
					}
				}
			}

			if( stage->bundle[ 1 ].image[ 0 ] != NULL ) {
				failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_MULTITEXTURE;
			}

			switch( stage->rgbGen ) {
			case CGEN_IDENTITY:
			case CGEN_IDENTITY_LIGHTING:
			case CGEN_VERTEX:
			case CGEN_EXACT_VERTEX:
				break;
			default:
				failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_RGBGEN;
				break;
			}

			switch( stage->alphaGen ) {
			case AGEN_IDENTITY:
			case AGEN_SKIP:
				break;
			default:
				failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_ALPHAGEN;
				break;
			}
		}
	}

	if( fogNum != 0 ) {
		failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_FOG;
	}

	if( surface->dlightBits != 0 ) {
		failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_DLIGHT;
	}

	if( shader->isSky || shader->sort == SS_ENVIRONMENT ) {
		failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_SKY;
	}

	if( shader->sort == SS_PORTAL ) {
		failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_PORTAL;
	}

	if( surface->numPoints < 0 || surface->numIndices < 0 ) {
		failedReasons |= 1U << PSP_STATIC_WORLD_REJECT_GEOMETRY;
	}

	*rejectMask = failedReasons;
	if( failedReasons ) {
		for( i = 0; i < PSP_STATIC_WORLD_REJECT_COUNT; i++ ) {
			if( failedReasons & ( 1U << i ) ) {
				return (pspStaticWorldReject_t)i;
			}
		}
	}

	vertexBytes = PSP_StaticWorldAlign16(
		(unsigned int)surface->numPoints * 24U );
	indexBytes = PSP_StaticWorldAlign16(
		(unsigned int)surface->numIndices * 2U );
	*cacheBytes = vertexBytes + indexBytes;

	return PSP_STATIC_WORLD_REJECT_COUNT;
}

/*
===============
PSP_StaticWorld_RegisterCvar

Called beside the other PSP renderer cvars. The value is deliberately not
latched, so classification or live drawing can be changed from the console
without rebuilding or restarting the map.
===============
*/
void PSP_StaticWorld_RegisterCvar( void )
{
	r_pspStaticWorld = ri.Cvar_Get( "r_pspStaticWorld", "0", 0 );

	if( r_pspStaticWorld->integer >= 2 ) {
		ri.Printf( PRINT_ALL,
			"PSP static world: live (r_pspStaticWorld %d)\n",
			r_pspStaticWorld->integer );
	} else if( r_pspStaticWorld->integer ) {
		ri.Printf( PRINT_ALL,
			"PSP static world: classify-only (r_pspStaticWorld %d)\n",
			r_pspStaticWorld->integer );
	} else {
		ri.Printf( PRINT_ALL,
			"PSP static world: disabled (r_pspStaticWorld %d)\n",
			r_pspStaticWorld->integer );
	}
}

/*
===============
PSP_StaticWorld_ClassifySurface

The first sighting of a face is classified and retained. Repeated sightings
are the dynamic measurement: they count the same face again without adding a
second unique entry. A later dlight or fog invalidates an otherwise qualifying
face for the rest of the map; a cache that cannot be used on one of its draws
is not honestly invariant.
===============
*/
void PSP_StaticWorld_ClassifySurface( const void *surfaceData,
	const void *shaderData, int fogNum )
{
	const srfSurfaceFace_t	*surface;
	const shader_t		*shader;
	pspStaticWorldEntry_t	*entry;
	pspStaticWorldReject_t	rejectReason;
	qboolean			found;
	qboolean			hadDlight;
	qboolean			hadFog;
	int			i;
	unsigned int		cacheBytes;
	unsigned int		rejectMask;

	if( !r_pspStaticWorld || !r_pspStaticWorld->integer ) {
		return;
	}

	surface = (const srfSurfaceFace_t *)surfaceData;
	shader = (const shader_t *)shaderData;
	pspStaticWorldDrawCalls++;

	entry = PSP_StaticWorldFindEntry( surface, &found );
	if( !entry ) {
		pspStaticWorldOverflow = qtrue;
		pspStaticWorldOverflowDrawCalls++;
		return;
	}

	if( found ) {
		hadDlight = surface->dlightBits != 0;
		hadFog = fogNum != 0;
		if( entry->qualifies && ( hadDlight || hadFog ) ) {
			entry->qualifies = qfalse;
			entry->rejectReason = hadDlight ?
				PSP_STATIC_WORLD_REJECT_DLIGHT : PSP_STATIC_WORLD_REJECT_FOG;
			pspStaticWorldQualifying--;
			pspStaticWorldCacheBytes -= entry->cacheBytes;
			if( hadDlight ) {
				pspStaticWorldRejects[ PSP_STATIC_WORLD_REJECT_DLIGHT ]++;
			}
			if( hadFog ) {
				pspStaticWorldRejects[ PSP_STATIC_WORLD_REJECT_FOG ]++;
			}
		}

		if( entry->qualifies ) {
			pspStaticWorldQualifyingDrawCalls++;
		}
		return;
	}

	entry->surface = surface;
	pspStaticWorldUnique++;
	cacheBytes = 0;
	rejectMask = 0;
	rejectReason = PSP_StaticWorldCheck( surface, shader, fogNum,
		&cacheBytes, &rejectMask );
	entry->rejectReason = rejectReason;
	entry->cacheBytes = cacheBytes;

	for( i = 0; i < PSP_STATIC_WORLD_REJECT_COUNT; i++ ) {
		if( rejectMask & ( 1U << i ) ) {
			pspStaticWorldRejects[ i ]++;
		}
	}

	if( rejectMask ) {
		entry->qualifies = qfalse;
		return;
	}

	entry->qualifies = qtrue;
	pspStaticWorldQualifying++;
	pspStaticWorldCacheBytes += cacheBytes;
	pspStaticWorldQualifyingDrawCalls++;
}

static void PSP_StaticWorldSubmitRun( void )
{
	const shaderStage_t	*stage;

	if( !pspStaticWorldRunShader )
		return;

	GL_Cull( pspStaticWorldRunShader->cullType );
	if( pspStaticWorldRunShader->polygonOffset ) {
		qglEnable( GL_POLYGON_OFFSET_FILL );
		qglPolygonOffset( r_offsetFactor->value, r_offsetUnits->value );
	}

	/* Multi-stage shaders are rejected at classification because one cached
	 * vertex buffer cannot serve two stages' texcoords. */
	stage = pspStaticWorldRunShader->stages[ 0 ];
	PSP_StaticWorld_BindImage( &stage->bundle[ 0 ] );
	GL_State( stage->stateBits );
	if( PSP_DrawStaticWorldSubmit() )
		pspStaticWorldCachedDraws++;

	if( pspStaticWorldRunShader->polygonOffset )
		qglDisable( GL_POLYGON_OFFSET_FILL );

	PSP_DrawStaticWorldEnd();
	pspStaticWorldRunShader = NULL;
}

void PSP_StaticWorld_Flush( void )
{
	PSP_StaticWorldSubmitRun();
}

/*
===============
PSP_StaticWorld_DrawSurface

Called only by RB_SurfaceFace. The old tessellation path remains the caller's
fallback. A cached face is separated from pending tessellation first, then
its native data is appended to the current same-shader run.
===============
*/
qboolean PSP_StaticWorld_DrawSurface( const void *surfaceData,
	const void *shaderData, int fogNum )
{
	const srfSurfaceFace_t	*surface;
	const shader_t			*shader;
	pspStaticWorldEntry_t	*entry;
	qboolean				found;

	if( !r_pspStaticWorld || !r_pspStaticWorld->integer ) {
		PSP_StaticWorld_Flush();
		return qfalse;
	}

	PSP_StaticWorld_ClassifySurface( surfaceData, shaderData, fogNum );
	if( r_pspStaticWorld->integer < 2 ) {
		PSP_StaticWorld_Flush();
		return qfalse;
	}

	surface = (const srfSurfaceFace_t *)surfaceData;
	shader = (const shader_t *)shaderData;
	entry = PSP_StaticWorldFindEntry( surface, &found );
	if( !found || !entry || !entry->qualifies ) {
		pspStaticWorldFallthroughNotEligible++;
		PSP_StaticWorld_Flush();
		return qfalse;
	}

	if( backEnd.currentEntity != &tr.worldEntity ) {
		pspStaticWorldFallthroughBmodel++;
		PSP_StaticWorld_Flush();
		return qfalse;
	}

	PSP_StaticWorldEnsureBounds( entry, surface );
	if( !PSP_StaticWorldInsideFrustum( entry ) ) {
		pspStaticWorldFallthroughFrustum++;
		PSP_StaticWorld_Flush();
		return qfalse;
	}

	if( !PSP_StaticWorldPackCache( entry, surface, shader ) ) {
		pspStaticWorldFallthroughCacheFull++;
		PSP_StaticWorld_Flush();
		return qfalse;
	}

	if( tess.numIndexes ) {
		PSP_StaticWorld_Flush();
		RB_EndSurface();
		RB_BeginSurface( tess.shader, tess.fogNum );
	}

	if( pspStaticWorldRunShader && pspStaticWorldRunShader != shader )
		PSP_StaticWorld_Flush();

	if( !PSP_DrawStaticWorldAppend( entry->cacheBlock,
		surface->numPoints,
		(unsigned short *)( (byte *)entry->cacheBlock +
			PSP_StaticWorldAlign16( (unsigned int)surface->numPoints * 24U ) ),
		surface->numIndices ) ) {
		pspStaticWorldFallthroughCacheFull++;
		PSP_StaticWorld_Flush();
		return qfalse;
	}

	pspStaticWorldRunShader = (shader_t *)shader;
	pspStaticWorldCachedFaces++;
	return qtrue;
}

/*
===============
PSP_StaticWorld_Reset

Surface pointers are hunk addresses. Clear the table at the end of every map
load so a later map can never find an old face at an aliased address.
===============
*/
void PSP_StaticWorld_Reset( void )
{
	if( !pspStaticWorldUnique && !pspStaticWorldDrawCalls &&
		!pspStaticWorldOverflow && !pspStaticWorldCacheInUse &&
		!pspStaticWorldRunShader ) {
		return;
	}

	Com_Memset( pspStaticWorldTable, 0, sizeof( pspStaticWorldTable ) );
	pspStaticWorldUnique = 0;
	pspStaticWorldQualifying = 0;
	pspStaticWorldCacheBytes = 0;
	pspStaticWorldDrawCalls = 0;
	pspStaticWorldQualifyingDrawCalls = 0;
	pspStaticWorldOverflowDrawCalls = 0;
	pspStaticWorldCacheInUse = 0;
	pspStaticWorldCacheFull = qfalse;
	pspStaticWorldCachedDraws = 0;
	pspStaticWorldCachedFaces = 0;
	pspStaticWorldFallthroughNotEligible = 0;
	pspStaticWorldFallthroughBmodel = 0;
	pspStaticWorldFallthroughFrustum = 0;
	pspStaticWorldFallthroughCacheFull = 0;
	Com_Memset( pspStaticWorldRejects, 0, sizeof( pspStaticWorldRejects ) );
	pspStaticWorldOverflow = qfalse;
	PSP_DrawStaticWorldEnd();
	pspStaticWorldRunShader = NULL;
}

/*
===============
PSP_StaticWorld_Report

The report is called from the existing PSP performance window. Classifier
counts are cumulative since the last map reset; live draw counts are reset
after each report while cache use remains cumulative for the map.
===============
*/
void PSP_StaticWorld_Report( void )
{
	unsigned int	qualifyingTenths;
	unsigned int	dynamicTenths;
	int		i;

	if( !r_pspStaticWorld || !r_pspStaticWorld->integer ) {
		return;
	}

	qualifyingTenths = pspStaticWorldUnique ?
		( pspStaticWorldQualifying * 1000U + pspStaticWorldUnique / 2U ) /
			pspStaticWorldUnique : 0;
	dynamicTenths = pspStaticWorldDrawCalls ?
		( pspStaticWorldQualifyingDrawCalls * 1000U +
			pspStaticWorldDrawCalls / 2U ) / pspStaticWorldDrawCalls : 0;

	Com_Printf( "PSP static world: unique %u, qualifying %u (%u.%u%%), cache %u KB (%u bytes)\n",
		pspStaticWorldUnique, pspStaticWorldQualifying,
		qualifyingTenths / 10U, qualifyingTenths % 10U,
		pspStaticWorldCacheBytes / 1024U, pspStaticWorldCacheBytes );
	Com_Printf( "PSP static world: draw calls %u, qualifying hits %u (%u.%u%%)\n",
		pspStaticWorldDrawCalls, pspStaticWorldQualifyingDrawCalls,
		dynamicTenths / 10U, dynamicTenths % 10U );

	for( i = 0; i < PSP_STATIC_WORLD_REJECT_COUNT; i++ ) {
		Com_Printf( "PSP static world: reject %-16s %u\n",
			pspStaticWorldRejectName[ i ], pspStaticWorldRejects[ i ] );
	}

	Com_Printf( "PSP static world: table overflow %s (%u draws not inserted)\n",
		pspStaticWorldOverflow ? "yes" : "no", pspStaticWorldOverflowDrawCalls );

	if( r_pspStaticWorld->integer >= 2 ) {
		Com_Printf( "PSP static world: cached draws %u, faces %u\n",
			pspStaticWorldCachedDraws, pspStaticWorldCachedFaces );
		Com_Printf( "PSP static world: fallthrough not eligible %u, bmodel %u, "
			"straddling frustum %u, cache full %u\n",
			pspStaticWorldFallthroughNotEligible,
			pspStaticWorldFallthroughBmodel,
			pspStaticWorldFallthroughFrustum,
			pspStaticWorldFallthroughCacheFull );
		Com_Printf( "PSP static world: cache in use %u KB (%u bytes) / "
			"%u KB (%u bytes)%s\n",
			pspStaticWorldCacheInUse / 1024U, pspStaticWorldCacheInUse,
			PSP_STATIC_WORLD_CACHE_CAP / 1024U,
			PSP_STATIC_WORLD_CACHE_CAP,
			pspStaticWorldCacheFull ? " (full)" : "" );

		pspStaticWorldCachedDraws = 0;
		pspStaticWorldCachedFaces = 0;
		pspStaticWorldFallthroughNotEligible = 0;
		pspStaticWorldFallthroughBmodel = 0;
		pspStaticWorldFallthroughFrustum = 0;
		pspStaticWorldFallthroughCacheFull = 0;
	}
}
