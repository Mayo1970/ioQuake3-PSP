/*
===========================================================================
PSP port - code/psp/con_psp.c

The observation channel for the XMB route: CON_Init/Shutdown/Print/Input
against pspDebugScreenPrintf (on-screen text) mirrored to a log file on the
Memory Stick. Modelled on the minimal shape of code/sys/con_passive.c;
con_log.c (the ring buffer CON_LogWrite/CON_LogRead feeds from) is
compiled unconditionally by shared_sources.cmake and is untouched.

The default PSP build now defers console/log output into a bounded RAM buffer
and flushes it after shutdown. A matched hardware A/B run showed that this
removes the periodic report-induced stutter while leaving file activity
unchanged. Disable PSP_TRACE_DEFER_CONSOLE for a crash-focused diagnostic
build that needs each line written through immediately.

The write-through path remains deliberately robust for hard-crash diagnosis:
sceIo has no usable fsync equivalent here, and a held-open fd can leave a
zero-byte log after an abnormal exit. Its cost is roughly 3-8 ms per line on a
Memory Stick.
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../sys/sys_local.h"

#include <pspdebug.h>
#include <pspiofilemgr.h>
#include <string.h>

#ifndef PSP_LOG_WRITETHROUGH
#define PSP_LOG_WRITETHROUGH 1
#endif

#ifdef PSP_TRACE_DEFER_CONSOLE
#define PSP_TRACE_DEFERRED_BYTES ( 256 * 1024 )
#endif

// Generation number in the log filename - see cmake/platforms/psp.cmake.
// Bump it whenever a run must not be confusable with the previous one: a
// stale q3psp<N>.log left by an earlier EBOOT reads exactly like a fresh one.
#ifndef PSP_LOG_GEN
#define PSP_LOG_GEN 5
#endif

// code/sys/sys_psp.c
extern char *Sys_PSP_BasePath( void );

/*
==================
Sys_PSP_IsDiagnosticMessage

The PSP reports are intentionally emitted with Com_Printf so they remain in
the normal engine log stream.  This classifier only controls the interactive
display; the caller still sends the complete message to Sys_Print, and thus
to q3psp*.log.
==================
*/
qboolean Sys_PSP_IsDiagnosticMessage( const char *msg )
{
#ifdef PSP_DIAGNOSTICS_LOG_ONLY
	static const char * const prefixes[] = {
		"PSP cgame syscall:",
		"PSP rprof:",
		"PSP perf:",
		"PSP frame:",
		"PSP paint:",
		"PSP audio:",
		"PSP sound load:",
		"PSP sound codec:",
		"PSP sound asset sampled:",
		"PSP file trace:",
		"PSP file trace slow:",
		"PSP pool:",
		"PSP heap ",
		"PSP hunk ",
		"PSP: heap ",
		"PSP ME "
	};
	const char *line = msg;

	if( !msg )
		return qfalse;

	/* Com_Printf normally supplies one report line, but handle a bundled
	   message as well so a diagnostic block cannot leak into chat. */
	while( *line )
	{
		unsigned int i;

		while( *line == '\n' || *line == '\r' )
			line++;

		for( i = 0; i < ARRAY_LEN( prefixes ); i++ )
		{
			if( !strncmp( line, prefixes[ i ], strlen( prefixes[ i ] ) ) )
				return qtrue;
		}

		line = strchr( line, '\n' );
		if( !line )
			break;
		line++;
	}
#else
	(void)msg;
#endif

	return qfalse;
}

static qboolean screenInit = qfalse;
static qboolean logReady   = qfalse;
static qboolean guOwnsDisplay = qfalse;
static char     logPath[ MAX_OSPATH ];

#if !PSP_LOG_WRITETHROUGH
static SceUID logFd = -1;
#endif

#ifdef PSP_TRACE_DEFER_CONSOLE
static char deferredLog[ PSP_TRACE_DEFERRED_BYTES ];
static int deferredLogLen;
static qboolean deferredLogOverflow;
#endif

/*
==================
CON_Init

The log path cannot be hardcoded: it depends on where the EBOOT was
launched from, and a PSP Go has no ms0: at all (Q3PORT.md 5, "All I/O
fails on a PSP Go"). Sys_PSP_ResolveBasePath runs in main() before
CON_Init, so the resolved base is already available here.
==================
*/
void CON_Init( void )
{
#ifdef PSP_TRACE_DEFER_CONSOLE
	deferredLogLen = 0;
	deferredLogOverflow = qfalse;
#endif

	if( !screenInit )
	{
		pspDebugScreenInit();
		screenInit = qtrue;
	}

	/*
	 The filename carries a generation number on purpose. Three hardware rounds
	 were lost to an ambiguity the log itself could not resolve: a run that
	 never happened leaves the PREVIOUS run's q3psp.log sitting on the stick,
	 which is indistinguishable from a run that happened and stopped early.
	 A new filename cannot be faked - if q3psp2.log does not exist after a
	 launch, this binary did not run. Bump the number whenever that question
	 comes up again.
	*/
	// The Memory Stick log file is a debug-build-only artifact.
#ifndef NDEBUG
	SceUID fd;

	Com_sprintf( logPath, sizeof( logPath ), "%s/q3psp%d.log", Sys_PSP_BasePath(), PSP_LOG_GEN );

	// Truncate once here; every later open appends.
	fd = sceIoOpen( logPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777 );

	if( fd < 0 )
	{
		logReady = qfalse;
		pspDebugScreenPrintf( "CON_Init: cannot open '%s' (0x%08X)\n", logPath, (unsigned int)fd );
		return;
	}

	logReady = qtrue;

#if PSP_LOG_WRITETHROUGH
	sceIoClose( fd );
#else
	logFd = fd;
#endif

	pspDebugScreenPrintf( "CON_Init: logging to %s\n", logPath );
#endif
}

/*
==================
CON_Shutdown
==================
*/
void CON_Shutdown( void )
{
#ifdef PSP_STUTTER_TRACE
	/* The trace is deliberately written after the measured workload. */
	Sys_PSP_StutterTraceDump();
#endif

#ifdef PSP_TRACE_DEFER_CONSOLE
	if( logReady && ( deferredLogLen > 0 || deferredLogOverflow ) )
	{
#if PSP_LOG_WRITETHROUGH
		SceUID deferredFd = sceIoOpen( logPath, PSP_O_WRONLY | PSP_O_APPEND, 0777 );
		if( deferredFd >= 0 )
		{
			sceIoWrite( deferredFd, deferredLog, deferredLogLen );
			if( deferredLogOverflow )
				sceIoWrite( deferredFd, "PSP deferred console buffer overflow\n",
					sizeof( "PSP deferred console buffer overflow\n" ) - 1 );
			sceIoClose( deferredFd );
		}
#else
		if( logFd >= 0 )
		{
			sceIoWrite( logFd, deferredLog, deferredLogLen );
			if( deferredLogOverflow )
				sceIoWrite( logFd, "PSP deferred console buffer overflow\n",
					sizeof( "PSP deferred console buffer overflow\n" ) - 1 );
		}
#endif
	}
#endif

#if !PSP_LOG_WRITETHROUGH
	if( logFd >= 0 )
	{
		sceIoClose( logFd );
		logFd = -1;
	}
#endif

	logReady = qfalse;
}

/*
==================
CON_Input

No keyboard/OSK this session (Session 9 for danzeff-style input).
==================
*/
char *CON_Input( void )
{
	return NULL;
}

/*
==================
CON_LogAppend
==================
*/
static void CON_LogAppend( const char *msg, int len )
{
	if( !logReady || len <= 0 )
		return;

#ifdef PSP_TRACE_DEFER_CONSOLE
	if( deferredLogLen < PSP_TRACE_DEFERRED_BYTES )
	{
		int copyLen = PSP_TRACE_DEFERRED_BYTES - deferredLogLen;
		if( copyLen > len )
			copyLen = len;
		memcpy( deferredLog + deferredLogLen, msg, copyLen );
		deferredLogLen += copyLen;
		if( copyLen != len )
			deferredLogOverflow = qtrue;
	}
	else
		deferredLogOverflow = qtrue;
	return;
#endif

#if PSP_LOG_WRITETHROUGH
	{
		SceUID fd = sceIoOpen( logPath, PSP_O_WRONLY | PSP_O_APPEND, 0777 );

		if( fd < 0 )
			return;

		sceIoWrite( fd, msg, len );
		sceIoClose( fd );
	}
#else
	if( logFd >= 0 )
		sceIoWrite( logFd, msg, len );
#endif
}

/*
==================
CON_SetDisplayOwnedByGU

Called by code/psp/psp_glimp.c around sceGuInit/sceGuTerm (Session 5).

pspDebugScreen renders straight into VRAM at offset 0, and offset 0 is
inside the range vramalloc hands the GU its framebuffers out of. Leaving it
live once the GE owns the display means both write the same memory, so the
on-screen half of this console has to shut off. The Memory Stick log is
unaffected, and it is the half that actually reports hardware runs.
==================
*/
void CON_SetDisplayOwnedByGU( qboolean owned )
{
	guOwnsDisplay = owned;
}

/*
==================
CON_Print

The log append comes FIRST: in the default build this is a bounded RAM copy;
the write-through diagnostic path still leaves the line on the stick before
the display call.

Q3 colour codes (^1..^7) pass through as-is - an ANSI translator only
matters once psplink is the channel.
==================
*/
void CON_Print( const char *msg )
{
	if( !msg || !*msg )
		return;

	CON_LogAppend( msg, (int)strlen( msg ) );

	if( Sys_PSP_IsDiagnosticMessage( msg ) )
		return;

	if( guOwnsDisplay )
		return;

	if( !screenInit )
	{
		pspDebugScreenInit();
		screenInit = qtrue;
	}

	pspDebugScreenPrintf( "%s", msg );
}
