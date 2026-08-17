/*
===========================================================================
PSP port - code/psp/psp_snd.c

Session 8: sceAudio backend for the portable software mixer (snd_dma.c).

Model
-----
Quake's base sound system is a ring buffer the engine paints into ahead of
playback; the backend only has to drain it and report how far playback has
got (SNDDMA_GetDMAPos, in mono samples). sceAudio is the opposite shape -
sceAudioOutputBlocking is a push that blocks until the previous buffer has
been consumed. One dedicated thread bridges the two: it converts a chunk out
of the ring into a staging buffer, pushes it, and blocks. The blocking push
IS the clock, which is why nothing here calls sceDisplayWaitVblankStart or
reads a timer.

  ring (dma.buffer, dma.speed Hz)  ->  staging (44100 Hz)  ->  sceAudio ch
        painted by the main thread       audio thread          hardware DMA

Rate
----
The GE audio hardware only accepts 44100 Hz on a normal channel, so the
thread upsamples by an integer ratio - s_khz 11 (the PSP default) means one
input frame is written four times. Restricting s_khz to 11/22/44 keeps that
ratio exact: no phase accumulator and no fractional resampler. At 11 kHz,
S_LoadSound resamples the 22050 Hz baseq3 assets at load time, trading that
one-time cost for roughly half the steady-state mixer work.

This is Quake3PSP-mirror/unix/psp_snddma.cpp's arrangement (its
inputSamplesPerOutputSample and its fillOutputBuffer), rebuilt on a plain
sceAudio channel instead of pspaudiolib. pspaudiolib reserves all four
hardware channels and runs four threads pushing 1024 frames each, three of
them silence, on a console with no CPU to spare; one channel and one thread
is what the mixer actually needs, since the engine has already mixed music,
effects and cinematic audio into the single ring.

Threading and cache
-------------------
- The audio thread runs at priority 0x12, above the main thread's 0x20, so a
  slow frame cannot starve it.
- Staging is double-buffered: the thread fills buffer B while the hardware is
  still reading buffer A.
- The staging buffers are 64-byte aligned and a multiple of 64 bytes long,
  and are written back before each push. pspaudiolib does not do this and
  works, so the kernel evidently handles coherency for sceAudio, but a 4 KB
  writeback every 23 ms is not measurable and rule 4 of the platform brief is
  not worth arguing with.

Underrun
--------
The port's own map load takes tens of seconds (Q3PORT.md Session 7:
CL_InitCGame 28.83 s), during which the main thread paints nothing. Without a
guard the ring simply loops, so a stall plays the last 0.74 s of audio over
and over at full volume. The thread therefore refuses to play past what the
mixer has actually written, and emits silence instead.

That high-water mark is taken in SNDDMA_Submit rather than read from
s_paintedtime directly, because s_paintedtime is not comparable to this
backend's frame counter in absolute terms. S_GetSoundtime reconstructs
s_soundtime from a wrap counter it admits it can miscount ("it is possible to
miscount buffers if it has wrapped twice between calls to S_Update" -
snd_dma.c:1258), and a stall of more than one ring period does exactly that.
The error is always a whole number of ring lengths, so the engine keeps
painting the correct ring slots and only the absolute value drifts. Sampling
the paint-ahead distance modulo the ring, once per S_Update, converts it back
into this file's counter space and is immune to that drift.

Underruns are counted and reported from the main thread (see
SNDDMA_GetDMAPos): with no OSK there is no console to type a stats command
into, and "does it click under load" is exactly the question this session's
gate asks.
===========================================================================
*/

#include <string.h>

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspaudio.h>

#include "../client/snd_local.h"

#ifdef PSP_ME_AUDIO_A6
void PSP_ME_AudioA6_ReportAndReset( void );
#endif

// Hardware output rate. Fixed: a normal sceAudio channel accepts nothing else.
#define PSP_SND_OUT_FREQ		44100

// Frames per sceAudio push. Must be a multiple of 64 (PSP_AUDIO_SAMPLE_MIN /
// the 64-byte alignment rule in pspaudio.h). 1024 frames is ~23 ms.
#define PSP_SND_OUT_FRAMES		1024

// Ring capacity in frames. A multiple of PSP_SND_OUT_FRAMES so a chunk read
// can never straddle the wrap for ratio 1, and a power of two so the
// remainder is a mask. 32768 frames is 1.49 s at 22050 Hz (2.97 s at the
// default 11025 Hz) and 128 KB of .bss.
// Capacity is not latency: latency is s_mixahead (0.5 s on PSP by default).
#define PSP_SND_RING_FRAMES		32768

#define PSP_SND_THREAD_PRIO		0x12
#define PSP_SND_THREAD_STACK	16384

// One stereo 16-bit frame is exactly one 32-bit word; the copy below moves
// words, not shorts.
typedef unsigned int pspSndFrame_t;

static pspSndFrame_t	psp_sndRing[ PSP_SND_RING_FRAMES ] __attribute__((aligned(64)));
static pspSndFrame_t	psp_sndOut[ 2 ][ PSP_SND_OUT_FRAMES ] __attribute__((aligned(64)));

static int				psp_sndChannel = -1;
static int				psp_sndThread  = -1;
static volatile int		psp_sndRun     = 0;
static qboolean			psp_sndInited  = qfalse;

// Input frames consumed per push, i.e. PSP_SND_OUT_FRAMES / ratio.
static int				psp_sndRatio  = 2;
static unsigned int		psp_sndChunk  = PSP_SND_OUT_FRAMES / 2;

// Monotonic count of input frames handed to the hardware. Written only by the
// audio thread, read by the main thread; a 32-bit aligned load is atomic on
// Allegrex, so no lock is needed for either direction.
static volatile unsigned int	psp_sndPlayed = 0;

// Frame, in psp_sndPlayed's units, up to which the mixer has painted. Written
// only by the main thread (SNDDMA_Submit), read by the audio thread.
static volatile unsigned int	psp_sndValid = 0;

// Underrun accounting, reported from the main thread.
static volatile int		psp_sndUnderruns = 0;

static cvar_t			*s_khz;

/*
===========================================================================
Session 10 step 1a - paint cost instrumentation

The underrun counter above increments whenever psp_sndValid has not advanced
far enough, which happens when the MAIN THREAD DID NOT PAINT. It says nothing
about how long painting takes. Session 9's log showed +2733 underruns across
a 57.08 s CL_InitCGame - 57.08 s / 23.22 ms per push is ~2458 - so that whole
burst was a blocked main thread, with mix cost contributing nothing.

The gameplay burst (+88 in a >=5 s window, ~2.0 s of silence) has two possible
causes the log cannot separate: painting is expensive, or S_Update_ is simply
called too rarely. This block measures both, so the next decision is made on a
number instead of a guess.

Read it as:
  paint us/call and share%   -> how much CPU S_PaintChannels actually costs
  interval mean/max ms       -> how often the main thread gets round to it
A large share means the Media Engine offload is worth building. A small share
with a large max interval means it is the wrong tool and pacing is the bug.

All counters are written only by the main thread (S_Update_ and this file's
report both run there), so no synchronisation is needed.
===========================================================================
*/
#define PSP_SND_PROFILE			1
#define PSP_SND_PROFILE_MS		5000

#if PSP_SND_PROFILE

static unsigned int		psp_pfBeginUs    = 0;	// timestamp of the current paint
static unsigned int		psp_pfLastUs     = 0;	// begin of the previous paint
static unsigned int		psp_pfWindowUs   = 0;	// wall clock at window start

static unsigned int		psp_pfPaintUs    = 0;	// summed paint time this window
static unsigned int		psp_pfMaxPaintUs = 0;
static unsigned int		psp_pfIntervalUs = 0;	// summed gap between paints
static unsigned int		psp_pfMaxGapUs   = 0;
static unsigned int		psp_pfCalls      = 0;
static unsigned int		psp_pfSamples    = 0;
static unsigned int		psp_pfChannels   = 0;	// summed, for a mean

void PSP_SndPaintBegin( void )
{
	psp_pfBeginUs = sceKernelGetSystemTimeLow();

	if( psp_pfLastUs ) {
		const unsigned int gap = psp_pfBeginUs - psp_pfLastUs;

		psp_pfIntervalUs += gap;
		if( gap > psp_pfMaxGapUs ) {
			psp_pfMaxGapUs = gap;
		}
	}

	psp_pfLastUs = psp_pfBeginUs;

	if( !psp_pfWindowUs ) {
		psp_pfWindowUs = psp_pfBeginUs;
	}
}

void PSP_SndPaintEnd( int samples, int activeChannels )
{
	const unsigned int us = sceKernelGetSystemTimeLow() - psp_pfBeginUs;

	psp_pfPaintUs += us;
	if( us > psp_pfMaxPaintUs ) {
		psp_pfMaxPaintUs = us;
	}

	psp_pfCalls++;
	psp_pfSamples  += ( samples > 0 ) ? (unsigned int)samples : 0;
	psp_pfChannels += (unsigned int)activeChannels;
}

/*
===============
PSP_SndProfileReport

Called from SNDDMA_GetDMAPos, i.e. from S_Update_ on the main thread. That
placement is deliberate: a window in which the main thread never ran produces
no report at all, and the missing time reappears as the next window's max gap.
===============
*/
static void PSP_SndProfileReport( void )
{
	unsigned int now, windowUs;

	if( !psp_pfCalls ) {
		return;
	}

	now      = sceKernelGetSystemTimeLow();
	windowUs = now - psp_pfWindowUs;

	if( windowUs < PSP_SND_PROFILE_MS * 1000 ) {
		return;
	}

	Com_Printf( "PSP paint: %u calls in %u ms, %u us/call (max %u), share %u%%\n",
		psp_pfCalls, windowUs / 1000,
		psp_pfPaintUs / psp_pfCalls, psp_pfMaxPaintUs,
		windowUs ? ( psp_pfPaintUs * 100 ) / windowUs : 0 );

	Com_Printf( "PSP paint: interval mean %u ms max %u ms, "
		"%u samples/call, %u chans/call\n",
		( psp_pfCalls > 1 ) ? psp_pfIntervalUs / ( ( psp_pfCalls - 1 ) * 1000 ) : 0,
		psp_pfMaxGapUs / 1000,
		psp_pfSamples / psp_pfCalls,
		psp_pfChannels / psp_pfCalls );

#ifdef PSP_ME_AUDIO_A6
	PSP_ME_AudioA6_ReportAndReset();
#endif

	psp_pfWindowUs   = now;
	psp_pfPaintUs    = 0;
	psp_pfMaxPaintUs = 0;
	psp_pfIntervalUs = 0;
	psp_pfMaxGapUs   = 0;
	psp_pfCalls      = 0;
	psp_pfSamples    = 0;
	psp_pfChannels   = 0;
}

#else

void PSP_SndPaintBegin( void ) { }
void PSP_SndPaintEnd( int samples, int activeChannels ) { }

#endif

/*
===============
PSP_SndExpand

Writes frames * ratio output frames from frames input frames. ratio 1 is a
straight copy; 2 and 4 duplicate each frame, which is a zero-order hold - the
mirror's copySamples, and audibly fine for 22050 -> 44100 material.
===============
*/
static void PSP_SndExpand( pspSndFrame_t *out, const pspSndFrame_t *in,
	unsigned int frames, int ratio )
{
	if( ratio == 1 ) {
		Com_Memcpy( out, in, frames * sizeof( pspSndFrame_t ) );
		return;
	}

	while( frames-- ) {
		const pspSndFrame_t frame = *in++;
		int r = ratio;

		do {
			*out++ = frame;
		} while( --r );
	}
}

/*
===============
PSP_SndThread

Drains the ring for as long as SNDDMA_Shutdown has not cleared psp_sndRun.
sceAudioOutputBlocking returns once the previous buffer has been consumed, so
this loop paces itself and must never be given anything else to do.
===============
*/
static int PSP_SndThread( SceSize args, void *argp )
{
	int bufIndex = 0;

	while( psp_sndRun ) {
		pspSndFrame_t	*out    = psp_sndOut[ bufIndex ];
		unsigned int	played  = psp_sndPlayed;
		unsigned int	pos     = played & ( PSP_SND_RING_FRAMES - 1 );

		// Has the mixer painted this far yet? Both counters are frames in the
		// same space, so the signed difference is wrap-safe.
		if( (int)( psp_sndValid - ( played + psp_sndChunk ) ) < 0 ) {
			Com_Memset( out, 0, sizeof( psp_sndOut[ 0 ] ) );
			psp_sndUnderruns++;
		} else {
			unsigned int avail = PSP_SND_RING_FRAMES - pos;

			if( avail >= psp_sndChunk ) {
				PSP_SndExpand( out, &psp_sndRing[ pos ], psp_sndChunk, psp_sndRatio );
			} else {
				// Only reachable if the chunk stops dividing the ring size.
				PSP_SndExpand( out, &psp_sndRing[ pos ], avail, psp_sndRatio );
				PSP_SndExpand( out + avail * psp_sndRatio, &psp_sndRing[ 0 ],
					psp_sndChunk - avail, psp_sndRatio );
			}
		}

		psp_sndPlayed = played + psp_sndChunk;

		sceKernelDcacheWritebackRange( out, sizeof( psp_sndOut[ 0 ] ) );
		sceAudioOutputBlocking( psp_sndChannel, PSP_AUDIO_VOLUME_MAX, out );

		bufIndex ^= 1;
	}

	return 0;
}

/*
===============
SNDDMA_Init
===============
*/
qboolean SNDDMA_Init( void )
{
	int speed;

	if( psp_sndInited ) {
		return qtrue;
	}

	s_khz = Cvar_Get( "s_khz", "11", CVAR_ARCHIVE | CVAR_LATCH );

	switch( s_khz->integer ) {
		case 11: speed = 11025; break;
		case 44: speed = 44100; break;
		default: speed = 22050; break;
	}

	psp_sndRatio = PSP_SND_OUT_FREQ / speed;
	psp_sndChunk = PSP_SND_OUT_FRAMES / psp_sndRatio;

	Com_Memset( psp_sndRing, 0, sizeof( psp_sndRing ) );
	Com_Memset( psp_sndOut, 0, sizeof( psp_sndOut ) );

	psp_sndPlayed    = 0;
	psp_sndValid     = 0;
	psp_sndUnderruns = 0;

	dma.channels         = 2;
	dma.samplebits       = 16;
	dma.isfloat          = 0;
	dma.speed            = speed;
	dma.fullsamples      = PSP_SND_RING_FRAMES;
	dma.samples          = PSP_SND_RING_FRAMES * 2;	// mono samples, as dma_t documents
	dma.submission_chunk = 1;						// the ring is drained continuously
	dma.buffer           = (byte *)psp_sndRing;

#ifdef PSP_SND_MIX_ASM
	/* The assembly arm is never allowed to fail open into distorted audio. */
	if( !PSP_SndMixAsmSelfTest() ) {
		Com_Printf( "PSP audio: mix-asm self-test FAILED; audio disabled\n" );
		dma.buffer = NULL;
		return qfalse;
	}
#endif

	psp_sndChannel = sceAudioChReserve( PSP_AUDIO_NEXT_CHANNEL,
		PSP_SND_OUT_FRAMES, PSP_AUDIO_FORMAT_STEREO );
	if( psp_sndChannel < 0 ) {
		Com_Printf( "PSP audio: sceAudioChReserve failed (0x%08X)\n", psp_sndChannel );
		dma.buffer = NULL;
		return qfalse;
	}

	psp_sndThread = sceKernelCreateThread( "q3audio", PSP_SndThread,
		PSP_SND_THREAD_PRIO, PSP_SND_THREAD_STACK, PSP_THREAD_ATTR_USER, NULL );
	if( psp_sndThread < 0 ) {
		Com_Printf( "PSP audio: sceKernelCreateThread failed (0x%08X)\n", psp_sndThread );
		sceAudioChRelease( psp_sndChannel );
		psp_sndChannel = -1;
		dma.buffer = NULL;
		return qfalse;
	}

	psp_sndRun    = 1;
	psp_sndInited = qtrue;

	sceKernelStartThread( psp_sndThread, 0, NULL );

	Com_Printf( "PSP audio: channel %d, %d Hz stereo -> %d Hz (x%d), "
		"%d frame pushes, %d frame ring\n",
		psp_sndChannel, dma.speed, PSP_SND_OUT_FREQ, psp_sndRatio,
		PSP_SND_OUT_FRAMES, PSP_SND_RING_FRAMES );

	return qtrue;
}

/*
===============
SNDDMA_GetDMAPos

Playback position within the ring, in mono samples - what S_GetSoundtime
counts buffer wraps from.

Also the port's only main-thread hook, so the underrun report rides along
here: on change, at most once every five seconds, and silent when the mixer
is keeping up.
===============
*/
int SNDDMA_GetDMAPos( void )
{
	static int	reportedUnderruns = 0;
	static int	nextReportTime    = 0;

	if( !psp_sndInited ) {
		return 0;
	}

	if( psp_sndUnderruns != reportedUnderruns ) {
		const int now = Sys_Milliseconds();

		if( now >= nextReportTime ) {
			Com_Printf( "PSP audio: %d underruns (+%d)\n",
				psp_sndUnderruns, psp_sndUnderruns - reportedUnderruns );
			reportedUnderruns = psp_sndUnderruns;
			nextReportTime    = now + 5000;
		}
	}

#if PSP_SND_PROFILE
	PSP_SndProfileReport();
#endif

	return (int)( ( psp_sndPlayed & ( PSP_SND_RING_FRAMES - 1 ) ) * dma.channels );
}

/*
===============
SNDDMA_Shutdown
===============
*/
void SNDDMA_Shutdown( void )
{
	if( !psp_sndInited ) {
		return;
	}

	// Silence first: the thread may push one more buffer before it notices.
	Com_Memset( psp_sndRing, 0, sizeof( psp_sndRing ) );

	psp_sndRun = 0;

	if( psp_sndThread >= 0 ) {
		SceUInt timeout = 1000 * 1000;	// one blocking push is ~23 ms

		sceKernelWaitThreadEnd( psp_sndThread, &timeout );
		sceKernelDeleteThread( psp_sndThread );
		psp_sndThread = -1;
	}

	if( psp_sndChannel >= 0 ) {
		sceAudioChRelease( psp_sndChannel );
		psp_sndChannel = -1;
	}

	Com_Printf( "PSP audio: shutdown, %d underruns total\n", psp_sndUnderruns );

	dma.buffer    = NULL;
	psp_sndInited = qfalse;
}

/*
===============
SNDDMA_BeginPainting

Nothing to lock. The mixer writes the ring behind the playback cursor and the
audio thread only reads in front of it; the one case where they can meet is
S_Base_ClearSoundBuffer zeroing the whole ring, and the worst that produces
is silence, which is what it asked for.
===============
*/
void SNDDMA_BeginPainting( void )
{
}

/*
===============
SNDDMA_Submit

Nothing to hand over - the audio thread pulls. What this does is record how
far the mixer just painted, in the audio thread's own frame counter, which is
the underrun guard's high-water mark (see the note at the top of the file).

s_paintedtime is only usable modulo the ring, so the paint-ahead distance is
taken there and added to the current playback position. A distance in the top
half of the ring means s_paintedtime is BEHIND playback, i.e. the mixer has
already lost the race; marking nothing valid keeps that silent until
S_GetSoundtime resets s_paintedtime from s_soundtime on the next frame.
===============
*/
void SNDDMA_Submit( void )
{
	unsigned int played;
	unsigned int ahead;

	if( !psp_sndInited ) {
		return;
	}

	played = psp_sndPlayed;
	ahead  = ( (unsigned int)s_paintedtime - played ) & ( PSP_SND_RING_FRAMES - 1 );

	if( ahead > PSP_SND_RING_FRAMES / 2 ) {
		ahead = 0;
	}

	psp_sndValid = played + ahead;
}
