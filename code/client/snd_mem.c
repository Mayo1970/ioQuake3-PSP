/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

/*****************************************************************************
 * name:		snd_mem.c
 *
 * desc:		sound caching
 *
 * $Archive: /MissionPack/code/client/snd_mem.c $
 *
 *****************************************************************************/

#include "snd_local.h"
#include "snd_codec.h"

#ifdef __PSP__
#include "../psp/psp_pool.h"
#define PSP_STRINGIFY_INNER(x) #x
#define PSP_STRINGIFY(x) PSP_STRINGIFY_INNER(x)
#ifndef PSP_SOUND_MEGS
#define PSP_SOUND_MEGS 2
#endif
#define DEF_COMSOUNDMEGS PSP_STRINGIFY(PSP_SOUND_MEGS)
#else
#define DEF_COMSOUNDMEGS "8"
#endif

/*
===============================================================================

memory management

===============================================================================
*/

static	sndBuffer	*buffer = NULL;
#ifdef __PSP__
static	sndBuffer	*volatileBuffer = NULL;
#endif
static	sndBuffer	*freelist = NULL;
static	int inUse = 0;
static	int totalInUse = 0;

short *sfxScratchBuffer = NULL;
sfx_t *sfxScratchPointer = NULL;
int	   sfxScratchIndex = 0;

void	SND_free(sndBuffer *v) {
	*(sndBuffer **)v = freelist;
	freelist = (sndBuffer*)v;
	inUse += sizeof(sndBuffer);
}

sndBuffer*	SND_malloc(void) {
	sndBuffer *v;
	#ifdef __PSP__
	Sys_PSP_RenderProfileBegin( PSP_RPROF_SOUND_POOL_ALLOC );
	#endif
redo:
	if (freelist == NULL) {
		if ( !S_FreeOldestSound() ) {
			Com_Error( ERR_FATAL, "SND_malloc: sound cache exhausted" );
			return NULL;
		}
		goto redo;
	}

	inUse -= sizeof(sndBuffer);
	totalInUse += sizeof(sndBuffer);

	v = freelist;
	freelist = *(sndBuffer **)freelist;
	v->next = NULL;
	#ifdef __PSP__
	Sys_PSP_RenderProfileEnd( PSP_RPROF_SOUND_POOL_ALLOC );
	#endif
	return v;
}

void SND_setup(void) {
	sndBuffer *p;
	cvar_t	*cv;
	int scs, volatileScs = 0, heapScs;
	int i;

	cv = Cvar_Get( "com_soundMegs", DEF_COMSOUNDMEGS, CVAR_LATCH | CVAR_ARCHIVE );

	scs = (cv->integer*1536);
	if ( scs < 1 ) {
		scs = 1;
	}

	#ifdef __PSP__
	volatileBuffer = (sndBuffer *)PSP_PoolTakeSound();
	if( volatileBuffer )
		volatileScs = scs < PSP_SOUND_CHUNKS_PER_UNIT ? scs : PSP_SOUND_CHUNKS_PER_UNIT;
	#endif

	heapScs = scs - volatileScs;
	if( heapScs > 0 )
		buffer = malloc(heapScs * sizeof(sndBuffer) );
	if ( heapScs > 0 && !buffer ) {
		#ifdef __PSP__
		PSP_PoolReleaseSound( volatileBuffer );
		volatileBuffer = NULL;
		#endif
		Com_Error( ERR_FATAL, "snd alloc(%i) failed", heapScs*sizeof(sndBuffer) );
		return;
	}
	// allocate the stack based hunk allocator
	sfxScratchBuffer = malloc(SND_CHUNK_SIZE * sizeof(short) * 4);	//Hunk_Alloc(SND_CHUNK_SIZE * sizeof(short) * 4);
	if ( !sfxScratchBuffer ) {
		free( buffer );
		buffer = NULL;
		#ifdef __PSP__
		PSP_PoolReleaseSound( volatileBuffer );
		volatileBuffer = NULL;
		#endif
		Com_Error( ERR_FATAL, "snd sfxScratchBuffer failed" );
		return;
	}
	sfxScratchPointer = NULL;

	inUse = scs*sizeof(sndBuffer);
	totalInUse = 0;
	freelist = NULL;

	#ifdef __PSP__
	if( volatileScs > 0 )
	{
		p = volatileBuffer;
		for( i = 0; i < volatileScs; i++, p++ )
		{
			*(sndBuffer **)p = freelist;
			freelist = p;
		}
	}
	#endif
	for( i = 0, p = buffer; i < heapScs; i++, p++ )
	{
		*(sndBuffer **)p = freelist;
		freelist = p;
	}

	Com_Printf("Sound memory manager started: %d chunks, %d bytes (com_soundMegs %d; volatile %d chunks/%d KB, heap %d chunks/%d KB)\n",
		scs, scs * (int)sizeof(sndBuffer), cv->integer,
		volatileScs, volatileScs * (int)sizeof(sndBuffer) / 1024,
		heapScs, heapScs * (int)sizeof(sndBuffer) / 1024);
}

void SND_shutdown(void)
{
		free(sfxScratchBuffer);
		free(buffer);
		#ifdef __PSP__
		PSP_PoolReleaseSound( volatileBuffer );
		#endif
		sfxScratchBuffer = NULL;
		buffer = NULL;
		#ifdef __PSP__
		volatileBuffer = NULL;
		#endif
		freelist = NULL;
		inUse = 0;
		totalInUse = 0;
}

/*
================
ResampleSfx

resample / decimate to the current source rate
================
*/
static int ResampleSfx( sfx_t *sfx, int channels, int inrate, int inwidth, int samples, byte *data, qboolean compressed ) {
	int		outcount;
	int		srcsample;
	float	stepscale;
	int		i, j;
	int		sample, samplefrac, fracstep;
	int			part;
	sndBuffer	*chunk;
	
	stepscale = (float)inrate / dma.speed;	// this is usually 0.5, 1, or 2

	outcount = samples / stepscale;

	srcsample = 0;
	samplefrac = 0;
	fracstep = stepscale * 256 * channels;
	chunk = sfx->soundData;

	for (i=0 ; i<outcount ; i++)
	{
		srcsample += samplefrac >> 8;
		samplefrac &= 255;
		samplefrac += fracstep;
		for (j=0 ; j<channels ; j++)
		{
			if( inwidth == 2 ) {
				sample = ( ((short *)data)[srcsample+j] );
			} else {
				sample = (unsigned int)( (unsigned char)(data[srcsample+j]) - 128) << 8;
			}
			part = (i*channels+j)&(SND_CHUNK_SIZE-1);
			if (part == 0) {
				sndBuffer	*newchunk;
				newchunk = SND_malloc();
				if (chunk == NULL) {
					sfx->soundData = newchunk;
				} else {
					chunk->next = newchunk;
				}
				chunk = newchunk;
			}

			chunk->sndChunk[part] = sample;
		}
	}

	return outcount;
}

/*
================
ResampleSfx

resample / decimate to the current source rate
================
*/
static int ResampleSfxRaw( short *sfx, int channels, int inrate, int inwidth, int samples, byte *data ) {
	int			outcount;
	int			srcsample;
	float		stepscale;
	int			i, j;
	int			sample, samplefrac, fracstep;
	
	stepscale = (float)inrate / dma.speed;	// this is usually 0.5, 1, or 2

	outcount = samples / stepscale;

	srcsample = 0;
	samplefrac = 0;
	fracstep = stepscale * 256 * channels;

	for (i=0 ; i<outcount ; i++)
	{
		srcsample += samplefrac >> 8;
		samplefrac &= 255;
		samplefrac += fracstep;
		for (j=0 ; j<channels ; j++)
		{
			if( inwidth == 2 ) {
				sample = LittleShort ( ((short *)data)[srcsample+j] );
			} else {
				sample = (int)( (unsigned char)(data[srcsample+j]) - 128) << 8;
			}
			sfx[i*channels+j] = sample;
		}
	}
	return outcount;
}

//=============================================================================

/*
==============
S_LoadSound

The filename may be different than sfx->name in the case
of a forced fallback of a player specific sound
==============
*/
qboolean S_LoadSound( sfx_t *sfx )
{
	byte	*data;
	short	*samples;
	snd_info_t	info;
//	int		size;
#ifdef __PSP__
	unsigned int loadStart;
	unsigned int stageStart;
	unsigned int codecUs, pcmAllocUs, resampleUs;
#endif

	// load it in
	#ifdef __PSP__
	loadStart = Sys_PSP_RenderProfileNow();
	stageStart = loadStart;
	Sys_PSP_RenderProfileSoundAsset( sfx->soundName );
	Sys_PSP_RenderProfileBegin( PSP_RPROF_SOUND_CODEC );
	#endif
	data = S_CodecLoad(sfx->soundName, &info);
	#ifdef __PSP__
	Sys_PSP_RenderProfileEnd( PSP_RPROF_SOUND_CODEC );
	codecUs = Sys_PSP_RenderProfileNow() - stageStart;
	#ifdef PSP_FILE_TRACE
	Sys_PSP_FileTraceEvent( PSP_TRACE_SOUND_CODEC, codecUs, 0 );
	#endif
	#endif
	if(!data)
	{
		#ifdef PSP_FILE_TRACE
		Sys_PSP_FileTraceEvent( PSP_TRACE_SOUND_LAZY_LOAD,
			Sys_PSP_RenderProfileNow() - loadStart, 0 );
		#endif
		return qfalse;
	}

	if ( info.width == 1 ) {
		Com_DPrintf(S_COLOR_YELLOW "WARNING: %s is a 8 bit audio file\n", sfx->soundName);
	}

	if ( info.rate != 22050 ) {
		Com_DPrintf(S_COLOR_YELLOW "WARNING: %s is not a 22kHz audio file\n", sfx->soundName);
	}

	#ifdef __PSP__
	Sys_PSP_RenderProfileSoundLoad( sfx->soundName, info.rate, info.width,
		info.channels, info.size, sfx->pspLoadCount );
	stageStart = Sys_PSP_RenderProfileNow();
	Sys_PSP_RenderProfileBegin( PSP_RPROF_SOUND_PCM_ALLOC );
	#endif
	samples = Hunk_AllocateTempMemory(info.channels * info.samples * sizeof(short) * 2);
	#ifdef __PSP__
	Sys_PSP_RenderProfileEnd( PSP_RPROF_SOUND_PCM_ALLOC );
	pcmAllocUs = Sys_PSP_RenderProfileNow() - stageStart;
	#ifdef PSP_FILE_TRACE
	Sys_PSP_FileTraceEvent( PSP_TRACE_SOUND_PCM_ALLOC, pcmAllocUs,
		(unsigned int)info.size );
	#endif
	#endif

	sfx->lastTimeUsed = Com_Milliseconds()+1;

	// each of these compression schemes works just fine
	// but the 16bit quality is much nicer and with a local
	// install assured we can rely upon the sound memory
	// manager to do the right thing for us and page
	// sound in as needed

	#ifdef __PSP__
	stageStart = Sys_PSP_RenderProfileNow();
	Sys_PSP_RenderProfileBegin( PSP_RPROF_SOUND_RESAMPLE );
	#endif
	if( info.channels == 1 && sfx->soundCompressed == qtrue) {
		sfx->soundCompressionMethod = 1;
		sfx->soundData = NULL;
		sfx->soundLength = ResampleSfxRaw( samples, info.channels, info.rate, info.width, info.samples, data + info.dataofs );
		S_AdpcmEncodeSound(sfx, samples);
#if 0
	} else if (info.channels == 1 && info.samples>(SND_CHUNK_SIZE*16) && info.width >1) {
		sfx->soundCompressionMethod = 3;
		sfx->soundData = NULL;
		sfx->soundLength = ResampleSfxRaw( samples, info.channels, info.rate, info.width, info.samples, (data + info.dataofs) );
		encodeMuLaw( sfx, samples);
	} else if (info.channels == 1 && info.samples>(SND_CHUNK_SIZE*6400) && info.width >1) {
		sfx->soundCompressionMethod = 2;
		sfx->soundData = NULL;
		sfx->soundLength = ResampleSfxRaw( samples, info.channels, info.rate, info.width, info.samples, (data + info.dataofs) );
		encodeWavelet( sfx, samples);
#endif
	} else {
		sfx->soundCompressionMethod = 0;
		sfx->soundData = NULL;
		sfx->soundLength = ResampleSfx( sfx, info.channels, info.rate, info.width, info.samples, data + info.dataofs, qfalse );
	}
	#ifdef __PSP__
	Sys_PSP_RenderProfileEnd( PSP_RPROF_SOUND_RESAMPLE );
	resampleUs = Sys_PSP_RenderProfileNow() - stageStart;
	#ifdef PSP_FILE_TRACE
	Sys_PSP_FileTraceEvent( PSP_TRACE_SOUND_RESAMPLE, resampleUs,
		(unsigned int)info.samples );
	#endif
	#endif
	#ifdef __PSP__
	#ifdef PSP_FILE_TRACE
	Sys_PSP_FileTraceEvent( PSP_TRACE_SOUND_LAZY_LOAD,
		Sys_PSP_RenderProfileNow() - loadStart, (unsigned int)info.size );
	#else
	Com_Printf( "PSP sound timing: %s codec=%u tempAlloc=%u resample=%u total=%u us\n",
		sfx->soundName, codecUs, pcmAllocUs, resampleUs,
		Sys_PSP_RenderProfileNow() - loadStart );
	#endif
	#endif

	sfx->soundChannels = info.channels;
	
	Hunk_FreeTempMemory(samples);
	Hunk_FreeTempMemory(data);

	return qtrue;
}

void S_DisplayFreeMemory(void) {
	Com_Printf("%d bytes free sound buffer memory, %d total used\n", inUse, totalInUse);
}
