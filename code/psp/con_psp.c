// Console on pspDebugScreen, mirrored to a Memory Stick log (the XMB route's only channel).
// Output is deferred to RAM by default: per-line writes (3-8 ms each) caused report stutter.

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

// Log filename generation (cmake/platforms/psp.cmake); bump it so a stale log cannot pass as new.
#ifndef PSP_LOG_GEN
#define PSP_LOG_GEN 5
#endif

// code/sys/sys_psp.c
extern char *Sys_PSP_BasePath( void );

// Keeps PSP reports off the screen only; every message still reaches q3psp*.log.
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
		"PSP ME ",
		"PSP count [",
		"PSP miss:"
	};
	const char *line = msg;

	if( !msg )
		return qfalse;

	// A bundled multi-line message is checked line by line so no report leaks into chat.
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

// The log path follows the EBOOT's launch directory; a PSP Go has no ms0:.
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

	// Debug builds only. The generation number exists because a run that never happened
	// leaves the previous log on the stick, which reads like a run that stopped early.
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

void CON_Shutdown( void )
{
#ifdef PSP_STUTTER_TRACE
	// Written after the measured workload on purpose.
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

// No keyboard or OSK input.
char *CON_Input( void )
{
	return NULL;
}

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

// pspDebugScreen draws at VRAM offset 0, inside the GU framebuffers, so screen output stops
// while the GE owns the display (psp_glimp.c). The log is unaffected.
void CON_SetDisplayOwnedByGU( qboolean owned )
{
	guOwnsDisplay = owned;
}

// Log first, so a write-through build keeps the line even if the display call crashes.
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
