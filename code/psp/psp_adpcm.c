// LRU cache of decoded ADPCM chunks. Upstream keeps one decoded chunk, so with N channels every
// paint decodes N full chunks; measured ~1.5 ms per active channel on Q3DM11 fights.
#include "../client/snd_local.h"
#include "psp_adpcm.h"

#include <stdlib.h>

#define PSP_ADPCM_CACHE_ENTRIES	32
#define PSP_ADPCM_CHUNK_SAMPLES	( SND_CHUNK_SIZE * 4 )

typedef struct {
	const sndBuffer	*chunk;
	unsigned int	lastUse;
	short			*samples;
} pspAdpcmEntry_t;

static pspAdpcmEntry_t	pspAdpcmCache[ PSP_ADPCM_CACHE_ENTRIES ];
static short			*pspAdpcmMemory;
static unsigned int		pspAdpcmClock;
static unsigned int		pspAdpcmHits, pspAdpcmMisses;
static cvar_t			*s_pspAdpcmCache;
// The s_pspAdpcmCache 0 path: upstream's single decoded chunk, in sfxScratchBuffer.
static const sndBuffer	*pspAdpcmScratchChunk;

// Heap, not .bss: the partition outside the heap is the net modules' budget.
void PSP_AdpcmCacheInit( void )
{
	int	i;

	s_pspAdpcmCache = Cvar_Get( "s_pspAdpcmCache", "1", 0 );
	pspAdpcmScratchChunk = NULL;
	if( !pspAdpcmMemory )
		pspAdpcmMemory = malloc( PSP_ADPCM_CACHE_ENTRIES * PSP_ADPCM_CHUNK_SAMPLES * sizeof( short ) );
	for( i = 0; i < PSP_ADPCM_CACHE_ENTRIES; i++ )
	{
		pspAdpcmCache[ i ].chunk = NULL;
		pspAdpcmCache[ i ].lastUse = 0;
		pspAdpcmCache[ i ].samples = pspAdpcmMemory ? pspAdpcmMemory + i * PSP_ADPCM_CHUNK_SAMPLES : NULL;
	}
	Com_Printf( "PSP audio: ADPCM cache %d KB (%s)\n",
		pspAdpcmMemory ? (int)( PSP_ADPCM_CACHE_ENTRIES * PSP_ADPCM_CHUNK_SAMPLES * sizeof( short ) / 1024 ) : 0,
		pspAdpcmMemory ? "allocated" : "malloc failed, decoding per paint" );
}

void PSP_AdpcmCacheShutdown( void )
{
	int	i;

	for( i = 0; i < PSP_ADPCM_CACHE_ENTRIES; i++ )
		pspAdpcmCache[ i ].chunk = NULL;
	pspAdpcmScratchChunk = NULL;
	free( pspAdpcmMemory );
	pspAdpcmMemory = NULL;
}

const short *PSP_AdpcmSamples( sndBuffer *chunk )
{
	pspAdpcmEntry_t	*victim;
	int				i;

	if( !pspAdpcmMemory || !s_pspAdpcmCache || !s_pspAdpcmCache->integer )
	{
		if( chunk != pspAdpcmScratchChunk )
		{
			S_AdpcmGetSamples( chunk, sfxScratchBuffer );
			pspAdpcmScratchChunk = chunk;
			pspAdpcmMisses++;
		}
		else
		{
			pspAdpcmHits++;
		}
		return sfxScratchBuffer;
	}

	victim = &pspAdpcmCache[ 0 ];
	for( i = 0; i < PSP_ADPCM_CACHE_ENTRIES; i++ )
	{
		pspAdpcmEntry_t	*entry = &pspAdpcmCache[ i ];

		if( entry->chunk == chunk )
		{
			entry->lastUse = ++pspAdpcmClock;
			pspAdpcmHits++;
			return entry->samples;
		}
		if( entry->lastUse < victim->lastUse )
			victim = entry;
	}

	S_AdpcmGetSamples( chunk, victim->samples );
	victim->chunk = chunk;
	victim->lastUse = ++pspAdpcmClock;
	pspAdpcmMisses++;
	return victim->samples;
}

void PSP_AdpcmForget( const sndBuffer *chunk )
{
	int	i;

	if( chunk == pspAdpcmScratchChunk )
		pspAdpcmScratchChunk = NULL;
	for( i = 0; i < PSP_ADPCM_CACHE_ENTRIES; i++ )
	{
		if( pspAdpcmCache[ i ].chunk == chunk )
		{
			pspAdpcmCache[ i ].chunk = NULL;
			pspAdpcmCache[ i ].lastUse = 0;
		}
	}
}

// Printed with the frame report; the counters reset each window.
void PSP_AdpcmCacheReport( void )
{
	unsigned int	total = pspAdpcmHits + pspAdpcmMisses;

	Com_Printf( "PSP paint: ADPCM cache %s, %u hits, %u decodes (%u%% hits)\n",
		( pspAdpcmMemory && s_pspAdpcmCache && s_pspAdpcmCache->integer ) ? "on" : "off",
		pspAdpcmHits, pspAdpcmMisses, total ? ( pspAdpcmHits * 100 ) / total : 0 );
	pspAdpcmHits = 0;
	pspAdpcmMisses = 0;
}
