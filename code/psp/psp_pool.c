/*
===========================================================================
PSP port - code/psp/psp_pool.c

Volatile-partition allocator. See psp_pool.h for why this exists.
===========================================================================
*/

#include <pspkernel.h>
#include <pspsuspend.h>
#include <psppower.h>
#include <string.h>

#include "../qcommon/q_shared.h"
#include "../client/snd_local.h"
#include "psp_pool.h"

#ifndef PSP_SOUND_MEGS
#define PSP_SOUND_MEGS 2
#endif

/*
 Block header. 16 bytes so every payload inherits the 16-byte alignment the
 GE and the VFPU want, without a per-allocation alignment dance.

 size is the PAYLOAD size; the header is not counted in it.
*/
typedef struct pspPoolBlock_s {
	struct pspPoolBlock_s	*next;
	unsigned int		size;
	unsigned int		used;
	unsigned int		pad;
} pspPoolBlock_t;

#define PSP_POOL_HDR	( (int)sizeof( pspPoolBlock_t ) )
#define PSP_POOL_ALIGN	16

static char		*psp_poolBase;
static int		psp_poolSize;
static pspPoolBlock_t	*psp_poolFirst;
static qboolean		psp_poolReady;
static qboolean		psp_poolTried;
static void		*psp_soundReserve;

/*
===============
PSP_PoolInit

sceKernelVolatileMemLock hands back the whole partition-5 block. It is
paired with scePowerLock so the firmware cannot suspend into it while we
hold it - without that, a lid close returns to a pool full of whatever the
suspend image left behind, and every texture in it is garbage.
===============
*/
qboolean PSP_PoolInit( void )
{
	void	*ptr  = NULL;
	int	size  = 0;
	int	rc;

	if( psp_poolTried )
		return psp_poolReady;

	psp_poolTried = qtrue;

	rc = sceKernelVolatileMemLock( 0, &ptr, &size );

	if( rc < 0 || !ptr || size <= PSP_POOL_HDR )
	{
		Com_Printf( "PSP pool: sceKernelVolatileMemLock failed (0x%08X), "
			"textures stay on the heap\n", (unsigned int)rc );
		return qfalse;
	}

	scePowerLock( 0 );

	// Align the base up; the block itself is already page-aligned in
	// practice, but the arithmetic below assumes it.
	psp_poolBase = (char *)( ( (unsigned int)ptr + ( PSP_POOL_ALIGN - 1 ) ) &
		~( PSP_POOL_ALIGN - 1 ) );
	psp_poolSize = size - (int)( psp_poolBase - (char *)ptr );
	psp_poolSize &= ~( PSP_POOL_ALIGN - 1 );

	psp_poolFirst        = (pspPoolBlock_t *)psp_poolBase;
	psp_poolFirst->next  = NULL;
	psp_poolFirst->size  = (unsigned int)( psp_poolSize - PSP_POOL_HDR );
	psp_poolFirst->used  = 0;
	psp_poolFirst->pad   = 0;

	psp_poolReady = qtrue;

	/*
	 Reserve one sound unit before any texture can consume the pool. The
	 default sound budget is two units (~6.18 MB), so the second unit remains
	 in the normal heap while this first unit lives in volatile memory.
	*/
	if( PSP_SOUND_MEGS > 0 )
	{
		psp_soundReserve = PSP_PoolAlloc(
			(size_t)PSP_SOUND_CHUNKS_PER_UNIT * sizeof( sndBuffer ) );
		if( psp_soundReserve )
		{
			Com_Printf( "PSP pool: sound reservation %d KB; texture remainder %d KB\n",
				( PSP_SOUND_CHUNKS_PER_UNIT * (int)sizeof( sndBuffer ) ) / 1024,
				( psp_poolSize - PSP_POOL_HDR -
				  PSP_SOUND_CHUNKS_PER_UNIT * (int)sizeof( sndBuffer ) ) / 1024 );
		}
		else
		{
			Com_Printf( "PSP pool: sound reservation failed; textures use the full pool\n" );
		}
	}

	Com_Printf( "PSP pool: volatile partition locked, %d KB at %p\n",
		psp_poolSize / 1024, psp_poolBase );

	return qtrue;
}

void *PSP_PoolAlloc( size_t bytes )
{
	pspPoolBlock_t	*b;
	unsigned int	want;

	if( !psp_poolReady || !bytes )
		return NULL;

	want = ( (unsigned int)bytes + ( PSP_POOL_ALIGN - 1 ) ) & ~( PSP_POOL_ALIGN - 1 );

	for( b = psp_poolFirst; b; b = b->next )
	{
		if( b->used || b->size < want )
			continue;

		// Split only when the remainder can hold a header plus something
		// worth allocating; otherwise hand over the few spare bytes.
		if( b->size >= want + PSP_POOL_HDR + PSP_POOL_ALIGN )
		{
			pspPoolBlock_t	*rest =
				(pspPoolBlock_t *)( (char *)b + PSP_POOL_HDR + want );

			rest->next = b->next;
			rest->size = b->size - want - PSP_POOL_HDR;
			rest->used = 0;
			rest->pad  = 0;

			b->next = rest;
			b->size = want;
		}

		b->used = 1;

		return (char *)b + PSP_POOL_HDR;
	}

	return NULL;
}

void *PSP_PoolTakeSound( void )
{
	void *p = psp_soundReserve;

	psp_soundReserve = NULL;
	return p;
}

void PSP_PoolReleaseSound( void *p )
{
	/* Keep the segment reserved for the next SND_setup after a restart. */
	if( p && !psp_soundReserve )
		psp_soundReserve = p;
}

void PSP_PoolFree( void *p )
{
	pspPoolBlock_t	*b, *prev = NULL;

	if( !p || !PSP_PoolOwns( p ) )
		return;

	b = (pspPoolBlock_t *)( (char *)p - PSP_POOL_HDR );
	b->used = 0;

	/*
	 Coalesce forward, then find the predecessor and coalesce into it. The
	 list is address-ordered by construction (splitting only ever inserts
	 the remainder immediately after its parent), so a neighbour in the
	 list is a neighbour in memory.
	*/
	while( b->next && !b->next->used )
	{
		b->size += b->next->size + PSP_POOL_HDR;
		b->next  = b->next->next;
	}

	for( prev = psp_poolFirst; prev && prev->next != b; prev = prev->next )
		;

	if( prev && !prev->used )
	{
		prev->size += b->size + PSP_POOL_HDR;
		prev->next  = b->next;
	}
}

qboolean PSP_PoolOwns( const void *p )
{
	const char	*c = (const char *)p;

	return ( psp_poolReady && c >= psp_poolBase &&
		c < psp_poolBase + psp_poolSize ) ? qtrue : qfalse;
}

void PSP_PoolStats( int *usedBytes, int *totalBytes, int *largestFree )
{
	pspPoolBlock_t	*b;
	int		used = 0, largest = 0;

	for( b = psp_poolFirst; b; b = b->next )
	{
		if( b->used )
			used += (int)b->size + PSP_POOL_HDR;
		else if( (int)b->size > largest )
			largest = (int)b->size;
	}

	if( usedBytes )
		*usedBytes = used;
	if( totalBytes )
		*totalBytes = psp_poolSize;
	if( largestFree )
		*largestFree = largest;
}
