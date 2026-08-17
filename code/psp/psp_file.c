/*
===========================================================================
PSP port - code/psp/psp_file.c

Session 7. See psp_file.h for why this exists: the PSP runs out of stdio
handles at ~10, ioquake3 holds one open per pk3 forever, and baseq3 ships
nine.

Eviction policy is least-recently-used, which the mirror's ioctrl.c does
not do - FS_FreeHandle takes the first open slot it finds. LRU matters on a
Memory Stick: a map load reads thousands of files but nearly all of them
come from PAK0, so the pak being read stays resident and the reopen cost is
paid by the paks that are idle anyway.
===========================================================================
*/

#include "psp_file.h"
#include "../qcommon/qcommon.h"

#include <stdio.h>
#include <string.h>

typedef struct {
	FILE		*fp;			// NULL when the slot is evicted, not closed
	char		name[ MAX_OSPATH ];
	char		mode[ 8 ];
	long		pos;			// authoritative only while fp is NULL
	qboolean	used;
	qboolean	failed;			// sticky, reported by PSP_VF_Error
	unsigned int	stamp;			// LRU
#ifdef PSP_STUTTER_TRACE
	int		physicalSlot;
#endif
} pspVFile_t;

static pspVFile_t	pspVF[ PSP_VF_SLOTS ];
static int		pspVFOpenCount;		// physically open right now
static unsigned int	pspVFClock;

// Diagnostics, reported with the heap numbers.
static int		pspVFPeakOpen;
static int		pspVFEvictions;
static int		pspVFRestores;
static int		pspVFOpenFailures;
#ifdef PSP_STUTTER_TRACE
static int		pspVFPhysicalUsed[ PSP_VF_MAX_OPEN ];
#endif

static pspVFile_t *PSP_VF_Slot( int handle )
{
	if( handle < 1 || handle > PSP_VF_SLOTS )
		return NULL;

	if( !pspVF[ handle - 1 ].used )
		return NULL;

	return &pspVF[ handle - 1 ];
}

#ifdef PSP_STUTTER_TRACE
static int PSP_VF_HandleForSlot( const pspVFile_t *slot )
{
	int i;

	if( !slot )
		return 0;

	for( i = 0; i < PSP_VF_SLOTS; i++ )
	{
		if( &pspVF[ i ] == slot )
			return i + 1;
	}

	return 0;
}

static int PSP_VF_AllocatePhysicalSlot( void )
{
	int i;

	for( i = 0; i < PSP_VF_MAX_OPEN; i++ )
	{
		if( !pspVFPhysicalUsed[ i ] )
		{
			pspVFPhysicalUsed[ i ] = 1;
			return i;
		}
	}

	return -1;
}

static void PSP_VF_FreePhysicalSlot( pspVFile_t *f )
{
	if( f && f->physicalSlot >= 0 && f->physicalSlot < PSP_VF_MAX_OPEN )
		pspVFPhysicalUsed[ f->physicalSlot ] = 0;
	if( f )
		f->physicalSlot = -1;
}
#endif

/*
===============
PSP_VF_Evict

Closes the least recently used physically-open slot, remembering where its
file position was. "except" is the slot about to be used, which must not be
chosen - otherwise a restore could evict itself.
===============
*/
static void PSP_VF_Evict( const pspVFile_t *except )
{
	pspVFile_t	*victim = NULL;
	int		i;
#ifdef PSP_FILE_TRACE
	unsigned int traceStart;
#endif
#ifdef PSP_STUTTER_TRACE
	int traceHandle;
	int traceSlot;
	long tracePos;
#endif

	for( i = 0; i < PSP_VF_SLOTS; i++ )
	{
		pspVFile_t	*f = &pspVF[ i ];

		if( !f->used || !f->fp || f == except )
			continue;

		if( !victim || f->stamp < victim->stamp )
			victim = f;
	}

	if( !victim )
		return;

#ifdef PSP_FILE_TRACE
	traceStart = Sys_PSP_RenderProfileNow();
#endif
	#ifdef PSP_STUTTER_TRACE
	traceHandle = PSP_VF_HandleForSlot( victim );
	traceSlot = victim->physicalSlot;
	#endif
	victim->pos = ftell( victim->fp );
	#ifdef PSP_STUTTER_TRACE
	tracePos = victim->pos;
	#endif
	fclose( victim->fp );
	victim->fp = NULL;
	#ifdef PSP_STUTTER_TRACE
	PSP_VF_FreePhysicalSlot( victim );
	#endif

	pspVFOpenCount--;
	pspVFEvictions++;
#ifdef PSP_FILE_TRACE
	Sys_PSP_FileTraceEvent( PSP_TRACE_VF_EVICT,
		Sys_PSP_RenderProfileNow() - traceStart, 0 );
#endif
#ifdef PSP_STUTTER_TRACE
	Sys_PSP_StutterTraceRecord( PSP_STUTTER_PHASE_VF_EVICT,
		Sys_PSP_RenderProfileNow() - traceStart, traceHandle, traceSlot,
		0, SEEK_SET, (int)tracePos, 0, PSP_STUTTER_FLAG_EVICT,
		victim->name );
#endif
}

/*
===============
PSP_VF_Touch

Marks a slot as most recently used and, if it was evicted, brings it back:
reopen, then seek to exactly where it was. The seek is the part the mirror
omits.

Returns NULL if the reopen failed, which is a real failure - the caller
must not treat it as end-of-file.
===============
*/
static pspVFile_t *PSP_VF_Touch( pspVFile_t *f, qboolean initialOpen )
{
#ifdef PSP_FILE_TRACE
	unsigned int restoreStart;
	unsigned int openStart;
	unsigned int seekStart;
#endif
#ifdef PSP_STUTTER_TRACE
	int restoreResult;
	int traceHandle;
	long tracePos;
#endif
	qboolean restoring;

	if( !f )
		return NULL;

	restoring = initialOpen ? qfalse : qtrue;
	#ifdef PSP_STUTTER_TRACE
	traceHandle = PSP_VF_HandleForSlot( f );
	tracePos = f->pos;
	#endif
	f->stamp = ++pspVFClock;

	if( f->fp )
		return f;

#ifdef PSP_FILE_TRACE
	if( restoring )
		restoreStart = Sys_PSP_RenderProfileNow();
#endif
	while( pspVFOpenCount >= PSP_VF_MAX_OPEN )
	{
		int	before = pspVFOpenCount;

		PSP_VF_Evict( f );

		if( pspVFOpenCount == before )
			break;			// nothing evictable; try anyway
	}

#ifdef PSP_FILE_TRACE
	openStart = Sys_PSP_RenderProfileNow();
#endif
	f->fp = fopen( f->name, f->mode );
#ifdef PSP_STUTTER_TRACE
	if( f->fp )
		f->physicalSlot = PSP_VF_AllocatePhysicalSlot();
#endif
#ifdef PSP_FILE_TRACE
	Sys_PSP_FileTraceEvent(
		restoring ? PSP_TRACE_VF_RESTORE_OPEN : PSP_TRACE_VF_OPEN,
		Sys_PSP_RenderProfileNow() - openStart, 0 );
#endif
#ifdef PSP_STUTTER_TRACE
	Sys_PSP_StutterTraceRecord(
		restoring ? PSP_STUTTER_PHASE_RESTORE_OPEN : PSP_STUTTER_PHASE_VF_OPEN,
		Sys_PSP_RenderProfileNow() - openStart, traceHandle,
		f->physicalSlot, 0, SEEK_SET, (int)tracePos, f->fp ? 0 : -1,
		restoring ? PSP_STUTTER_FLAG_RESTORE : 0, f->name );
#endif

	if( !f->fp )
	{
		f->failed = qtrue;
		pspVFOpenFailures++;
#ifdef PSP_FILE_TRACE
		if( restoring )
			Sys_PSP_FileTraceEvent( PSP_TRACE_VF_RESTORE,
				Sys_PSP_RenderProfileNow() - restoreStart, 0 );
#endif
		return NULL;
	}

	if( f->pos > 0 )
	{
#ifdef PSP_FILE_TRACE
		seekStart = Sys_PSP_RenderProfileNow();
#endif
	#ifdef PSP_STUTTER_TRACE
	restoreResult = fseek( f->fp, f->pos, SEEK_SET );
	#else
	fseek( f->fp, f->pos, SEEK_SET );
	#endif
#ifdef PSP_FILE_TRACE
		Sys_PSP_FileTraceEvent( PSP_TRACE_VF_RESTORE_SEEK,
			Sys_PSP_RenderProfileNow() - seekStart, 0 );
#endif
#ifdef PSP_STUTTER_TRACE
		Sys_PSP_StutterTraceRecord( PSP_STUTTER_PHASE_RESTORE_SEEK,
			Sys_PSP_RenderProfileNow() - seekStart, traceHandle,
			f->physicalSlot, (int)f->pos, SEEK_SET, (int)tracePos,
			restoreResult, PSP_STUTTER_FLAG_RESTORE | PSP_STUTTER_FLAG_CHILD,
			f->name );
#endif
	}

	pspVFOpenCount++;
	pspVFRestores++;

	if( pspVFOpenCount > pspVFPeakOpen )
		pspVFPeakOpen = pspVFOpenCount;

#ifdef PSP_FILE_TRACE
	if( restoring )
		Sys_PSP_FileTraceEvent( PSP_TRACE_VF_RESTORE,
			Sys_PSP_RenderProfileNow() - restoreStart, 0 );
#endif
#ifdef PSP_STUTTER_TRACE
	if( restoring )
		Sys_PSP_StutterTraceRecord( PSP_STUTTER_PHASE_VF_RESTORE,
			Sys_PSP_RenderProfileNow() - restoreStart, traceHandle,
			f->physicalSlot, 0, SEEK_SET, (int)tracePos, 0,
			PSP_STUTTER_FLAG_RESTORE | PSP_STUTTER_FLAG_PARENT, f->name );
#endif

	return f;
}

/*
===============
PSP_VF_Open
===============
*/
int PSP_VF_Open( const char *name, const char *mode )
{
	pspVFile_t	*f = NULL;
	int		i, handle = 0;

	if( !name || !mode )
		return 0;

	for( i = 0; i < PSP_VF_SLOTS; i++ )
	{
		if( !pspVF[ i ].used )
		{
			f      = &pspVF[ i ];
			handle = i + 1;
			break;
		}
	}

	if( !f )
	{
		Com_Printf( S_COLOR_RED "PSP_VF_Open: all %d virtual handles in use, opening '%s'\n",
			PSP_VF_SLOTS, name );
		return 0;
	}

	Com_Memset( f, 0, sizeof( *f ) );

	Q_strncpyz( f->name, name, sizeof( f->name ) );
	Q_strncpyz( f->mode, mode, sizeof( f->mode ) );

	f->used = qtrue;
	f->pos  = 0;
#ifdef PSP_STUTTER_TRACE
	f->physicalSlot = -1;
#endif

	// Touch does the capacity check and the actual fopen.
	if( !PSP_VF_Touch( f, qtrue ) )
	{
		f->used = qfalse;
		return 0;
	}

	return handle;
}

int PSP_VF_Close( int handle )
{
	pspVFile_t	*f = PSP_VF_Slot( handle );

	if( !f )
		return -1;

	if( f->fp )
	{
		fclose( f->fp );
		f->fp = NULL;
	#ifdef PSP_STUTTER_TRACE
		PSP_VF_FreePhysicalSlot( f );
	#endif
		pspVFOpenCount--;
	}

	f->used = qfalse;

	return 0;
}

int PSP_VF_Read( int handle, void *buf, int size )
{
	pspVFile_t	*f;
	int		result;
#ifdef PSP_FILE_TRACE
	unsigned int traceStart;
#endif

	f = PSP_VF_Touch( PSP_VF_Slot( handle ), qfalse );

	if( !f )
		return 0;

#ifdef PSP_FILE_TRACE
	traceStart = Sys_PSP_RenderProfileNow();
#endif
	result = (int)fread( buf, 1, (size_t)size, f->fp );
#ifdef PSP_FILE_TRACE
	Sys_PSP_FileTraceEvent( PSP_TRACE_VF_READ,
		Sys_PSP_RenderProfileNow() - traceStart, (unsigned int)size );
#endif
#ifdef PSP_STUTTER_TRACE
	Sys_PSP_StutterTraceRecord(
		Sys_PSP_StutterTraceCurrentPhase( PSP_STUTTER_PHASE_VF_READ ),
		Sys_PSP_RenderProfileNow() - traceStart, handle, f->physicalSlot,
		size, 0, -1, result, 0, f->name );
#endif
	return result;
}

int PSP_VF_Write( int handle, const void *buf, int size )
{
	pspVFile_t	*f = PSP_VF_Touch( PSP_VF_Slot( handle ), qfalse );

	if( !f )
		return 0;

	return (int)fwrite( buf, 1, (size_t)size, f->fp );
}

long PSP_VF_Tell( int handle )
{
	pspVFile_t	*f = PSP_VF_Touch( PSP_VF_Slot( handle ), qfalse );

	if( !f )
		return -1;

	return ftell( f->fp );
}

int PSP_VF_Seek( int handle, long offset, int origin )
{
	pspVFile_t	*f;
	int		result;
#ifdef PSP_FILE_TRACE
	unsigned int traceStart;
#endif
#ifdef PSP_STUTTER_TRACE
	long savedPos;
#endif

	#ifdef PSP_STUTTER_TRACE
	f = PSP_VF_Slot( handle );
	if( f )
		savedPos = f->fp ? ftell( f->fp ) : f->pos;
	else
		savedPos = -1;
	#else
	f = PSP_VF_Touch( PSP_VF_Slot( handle ), qfalse );
	#endif

	#ifdef PSP_STUTTER_TRACE
	f = PSP_VF_Touch( f, qfalse );
	#endif

	if( !f )
		return -1;

#ifdef PSP_FILE_TRACE
	traceStart = Sys_PSP_RenderProfileNow();
#endif
	result = fseek( f->fp, offset, origin );
#ifdef PSP_FILE_TRACE
	Sys_PSP_FileTraceEvent( PSP_TRACE_VF_SEEK,
		Sys_PSP_RenderProfileNow() - traceStart, (unsigned int)offset );
#endif
#ifdef PSP_STUTTER_TRACE
	Sys_PSP_StutterTraceRecord(
		Sys_PSP_StutterTraceCurrentPhase( PSP_STUTTER_PHASE_VF_SEEK ),
		Sys_PSP_RenderProfileNow() - traceStart, handle, f->physicalSlot,
		(int)offset, origin, (int)savedPos, result, 0, f->name );
#endif
	return result;
}

int PSP_VF_Error( int handle )
{
	pspVFile_t	*f = PSP_VF_Slot( handle );

	if( !f )
		return 1;

	if( f->failed )
		return 1;

	return f->fp ? ferror( f->fp ) : 0;
}

/*
===============
PSP_VF_ReportInto

Folded into the PSP resource report rather than given its own console
command: the numbers only mean anything next to a map load, and that is
exactly where the report is already called from.

pspVFOpenFailures is the one to watch. It must stay 0 - a non-zero value
means PSP_VF_MAX_OPEN is still too high for whatever else this build holds
open, and the ceiling has to come down rather than the cache being blamed.
===============
*/
void PSP_VF_ReportInto( char *buf, int size )
{
	Com_sprintf( buf, size,
		"pak handles: %d open (peak %d of %d), %d evictions, %d restores, %d open failures",
		pspVFOpenCount, pspVFPeakOpen, PSP_VF_MAX_OPEN,
		pspVFEvictions, pspVFRestores, pspVFOpenFailures );
}
