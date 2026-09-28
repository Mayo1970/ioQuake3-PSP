// sys_unix.c counterpart: the Sys_* contract plus PSP boot (module info, exit callback, clock,
// Slim gate) after DaedalusX64 and the mirror. No XDG, fork, signals or mmap exist here.

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"
#include "../psp/psp_file.h"

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <pspfpu.h>
#include <pspge.h>
#include <pspsysmem.h>
#include <pspsuspend.h>
#include <kubridge.h>
#include <psprtc.h>
#include <psputility_sysparam.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>		// mallinfo - DIAGNOSTIC, see Sys_PSP_HeapReport
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#ifndef PSP_PERF_BUILD_ID
#define PSP_PERF_BUILD_ID "unlabeled"
#endif
#ifndef PSP_LOG_GEN
#define PSP_LOG_GEN 5
#endif

PSP_MODULE_INFO( "ioquake3", 0, 1, 0 );                              // attr 0 = user mode
PSP_MAIN_THREAD_ATTR( PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU );  // VFPU or sceGum crashes

// An explicit positive size (psp.cmake sets it). The hunk, zone, heap sound units, vertex
// arena and texture spill all come out of this one newlib heap.
#ifndef PSP_HEAP_KB
#define PSP_HEAP_KB 39936
#endif
PSP_HEAP_SIZE_KB( PSP_HEAP_KB );

// Heap MB that is not hunk; Sys_PSP_BuildBootCommandLine sets com_hunkMegs to heap minus this.
#ifndef PSP_HUNK_RESERVE_MB
#define PSP_HUNK_RESERVE_MB 12
#endif

// com_soundMegs is a unit count (1536 sndBuffers, ~3 MB each), not megabytes.
#ifndef PSP_SOUND_MEGS
#define PSP_SOUND_MEGS 1
#endif
#ifndef PSP_ZONE_MEGS
#define PSP_ZONE_MEGS 5
#endif

// The 256 KB default is too small: Sys_ListFiles puts a 16 KB list on the stack and recurses.
PSP_MAIN_THREAD_STACK_SIZE_KB( 512 );

// Last-resort probe candidates, in order. A PSP Go has no ms0: at all, so ef0:
// is tried first (on a Go both may exist; ef0: is where the EBOOT lives).
#ifdef STANDALONEOA
#define PSP_INSTALL_DIR "openarena"
#else
#define PSP_INSTALL_DIR "ioquake3"
#endif
#define PSP_EF0_BASE_PATH "ef0:/PSP/GAME/" PSP_INSTALL_DIR
#define PSP_MS0_BASE_PATH "ms0:/PSP/GAME/" PSP_INSTALL_DIR

// Exit callback thread: without it the HOME button's exit dialog hangs the app.
static volatile int psp_running = 1;

static int PSP_ExitCallback( int arg1, int arg2, void *common )
{
	psp_running = 0;
	return 0;
}

static int PSP_CallbackThread( SceSize args, void *argp )
{
	int cbid = sceKernelCreateCallback( "Exit Callback", PSP_ExitCallback, NULL );
	sceKernelRegisterExitCallback( cbid );
	sceKernelSleepThreadCB();
	return 0;
}

static void PSP_SetupExitCallback( void )
{
	int thid = sceKernelCreateThread( "update_thread", PSP_CallbackThread, 0x11, 0xFA0, 0, NULL );
	if( thid >= 0 )
		sceKernelStartThread( thid, 0, NULL );
}

qboolean Sys_PSP_Running( void )
{
	return psp_running ? qtrue : qfalse;
}

// The CWD depends on the launcher, so the base path is absolute: dirname(argv[0]), getcwd()
// (the mirror's way), then ef0:/ and ms0:/. The trace is kept until the log exists.
static char psp_basePath[ MAX_OSPATH ];
static qboolean psp_basePathResolved = qfalse;
static char psp_bootTrace[ 1024 ];
static char psp_argv0[ MAX_OSPATH ];

static void PSP_TraceAppend( const char *fmt, ... ) Q_PRINTF_FUNC( 1, 2 );

static void PSP_TraceAppend( const char *fmt, ... )
{
	va_list argptr;
	char    text[ 256 ];

	va_start( argptr, fmt );
	Q_vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	Q_strcat( psp_bootTrace, sizeof( psp_bootTrace ), text );
}

// sceIoGetstat, not stat(): it runs before the engine is up and reports the PSP error code.
static qboolean PSP_IsDirectory( const char *path )
{
	SceIoStat st;
	int       rc;

	memset( &st, 0, sizeof( st ) );

	rc = sceIoGetstat( path, &st );
	if( rc < 0 )
	{
		PSP_TraceAppend( "    reject '%s': sceIoGetstat 0x%08X\n", path, rc );
		return qfalse;
	}

	if( !FIO_S_ISDIR( st.st_mode ) )
	{
		PSP_TraceAppend( "    reject '%s': not a directory (st_mode 0x%04X)\n",
			path, (unsigned int)st.st_mode );
		return qfalse;
	}

	return qtrue;
}

// "ms0:/", "ef0:/", "host0:/", "disc0:/". A path without one is launcher-relative and useless.
static qboolean PSP_HasDevicePrefix( const char *path )
{
	const char *colon = strchr( path, ':' );

	// The prefix must come first and be non-empty: "ms0:" yes, "/foo:bar" no.
	return ( colon && colon != path && strchr( path, '/' ) != NULL &&
		colon < strchr( path, '/' ) ) ? qtrue : qfalse;
}

char *Sys_PSP_ResolveBasePath( const char *argv0 )
{
	char candidate[ MAX_OSPATH ];
	char argv0copy[ MAX_OSPATH ];

	if( psp_basePathResolved )
		return psp_basePath;

	psp_basePathResolved = qtrue;

	Q_strncpyz( psp_argv0, argv0 ? argv0 : "(null)", sizeof( psp_argv0 ) );
	PSP_TraceAppend( "PSP: resolving base path, argv[0] = '%s'\n", psp_argv0 );

	// 1. dirname(argv[0])
	if( argv0 && *argv0 )
	{
		Q_strncpyz( argv0copy, argv0, sizeof( argv0copy ) );

		if( PSP_HasDevicePrefix( argv0copy ) )
		{
			Q_strncpyz( candidate, Sys_Dirname( argv0copy ), sizeof( candidate ) );

			if( PSP_IsDirectory( candidate ) )
			{
				Q_strncpyz( psp_basePath, candidate, sizeof( psp_basePath ) );
				PSP_TraceAppend( "  accept argv[0] dirname: '%s'\n", psp_basePath );
				return psp_basePath;
			}
		}
		else
		{
			PSP_TraceAppend( "    reject argv[0]: no device prefix\n" );
		}
	}

	// 2. getcwd() - the mirror's mechanism, hardware-proven
	if( getcwd( candidate, sizeof( candidate ) - 1 ) != NULL )
	{
		candidate[ sizeof( candidate ) - 1 ] = '\0';

		if( PSP_HasDevicePrefix( candidate ) && PSP_IsDirectory( candidate ) )
		{
			Q_strncpyz( psp_basePath, candidate, sizeof( psp_basePath ) );
			PSP_TraceAppend( "  accept getcwd: '%s'\n", psp_basePath );
			return psp_basePath;
		}

		PSP_TraceAppend( "    reject getcwd '%s'\n", candidate );
	}
	else
	{
		PSP_TraceAppend( "    reject getcwd: returned NULL\n" );
	}

	// 3. Hardcoded install paths
	if( PSP_IsDirectory( PSP_EF0_BASE_PATH ) )
	{
		Q_strncpyz( psp_basePath, PSP_EF0_BASE_PATH, sizeof( psp_basePath ) );
		PSP_TraceAppend( "  accept probe: '%s'\n", psp_basePath );
		return psp_basePath;
	}

	if( PSP_IsDirectory( PSP_MS0_BASE_PATH ) )
	{
		Q_strncpyz( psp_basePath, PSP_MS0_BASE_PATH, sizeof( psp_basePath ) );
		PSP_TraceAppend( "  accept probe: '%s'\n", psp_basePath );
		return psp_basePath;
	}

	// Nothing validated. Use ms0: anyway so the engine produces a real
	// "file not found" against a nameable path instead of an empty one.
	Q_strncpyz( psp_basePath, PSP_MS0_BASE_PATH, sizeof( psp_basePath ) );
	PSP_TraceAppend( "  NO CANDIDATE VALIDATED - falling back to '%s'\n", psp_basePath );

	return psp_basePath;
}

// mallinfo is the newlib heap (hunk, zone, texture spill); sceKernelMaxFreeMemSize is the
// partition outside it. Both print because confusing the two is the classic sizing mistake.
void Sys_PSP_HeapReport( const char *where )
{
	struct mallinfo	mi = mallinfo();
	char		vf[ 160 ];

	Sys_Print( va( "PSP heap [%s]: arena %d KB, used %d KB, free %d KB, "
		"largest free %d KB, outside-heap %d KB\n",
		where ? where : "?",
		mi.arena    / 1024,
		mi.uordblks / 1024,
		mi.fordblks / 1024,
		mi.keepcost / 1024,
		(int)( sceKernelMaxFreeMemSize() / 1024 ) ) );

	// The hunk is calloc'd from the same heap, so read the two together; heap pressure is not
	// a hunk shortage. Hunk_MemoryRemaining reads 0 before Com_InitHunkMemory.
	Sys_Print( va( "PSP hunk [%s]: %d KB free of com_hunkMegs %d\n",
		where ? where : "?",
		Hunk_MemoryRemaining() / 1024,
		(int)Cvar_VariableValue( "com_hunkMegs" ) ) );

	// Lowest values are since the previous report; -1 means the pool does not exist yet.
	{
		int	zoneFree, zoneLowest, zoneLargest, soundFree, soundLowest;

		Z_PSP_FreeMemory( &zoneFree, &zoneLowest, &zoneLargest );
		SND_PSP_FreeMemory( &soundFree, &soundLowest );
		Sys_Print( va( "PSP zone [%s]: %d KB free, lowest %d KB, largest block %d KB; "
			"sound %d KB free, lowest %d KB\n",
			where ? where : "?",
			zoneFree < 0 ? -1 : zoneFree / 1024,
			zoneLowest < 0 ? -1 : zoneLowest / 1024,
			zoneLargest < 0 ? -1 : zoneLargest / 1024,
			soundFree < 0 ? -1 : soundFree / 1024,
			soundLowest < 0 ? -1 : soundLowest / 1024 ) );
	}

	// A map load once failed on file handles with heap to spare, so both are logged together.
	PSP_VF_ReportInto( vf, sizeof( vf ) );
	Sys_Print( va( "PSP %s\n", vf ) );

	// File and load counters since the previous report, so each load phase reads on its own.
	Sys_PSP_CountReport( where ? where : "?", 0 );
}

// Measures whether the loader zeroed .bss (libcglue's heap setup depends on it). Must be the
// first statement in main(); results are printed once the log exists.
#define PSP_BSS_BUCKETS 16

static int          psp_bssBucketNonZero[ PSP_BSS_BUCKETS ];
static unsigned int psp_bssStart, psp_bssEnd, psp_bssFirstNonZero, psp_bssNonZeroTotal;

void Sys_PSP_ScanBss( void )
{
	extern char __bss_start;
	extern char _end;

	const unsigned int *p, *begin, *end;
	unsigned int  span, bucketSpan, i;
	int           bucket[ PSP_BSS_BUCKETS ];
	unsigned int  firstNonZero = 0;
	unsigned int  total = 0;

	begin = (const unsigned int *)&__bss_start;
	end   = (const unsigned int *)&_end;
	span  = (unsigned int)( (const char *)end - (const char *)begin );
	bucketSpan = span / PSP_BSS_BUCKETS + 1;

	for( i = 0; i < PSP_BSS_BUCKETS; i++ )
		bucket[ i ] = 0;

	for( p = begin; p < end; p++ )
	{
		if( *p )
		{
			unsigned int off = (unsigned int)( (const char *)p - (const char *)begin );
			unsigned int b   = off / bucketSpan;

			if( b >= PSP_BSS_BUCKETS )
				b = PSP_BSS_BUCKETS - 1;

			bucket[ b ]++;
			total++;

			if( !firstNonZero )
				firstNonZero = (unsigned int)p;
		}
	}

	// Only now write results - the scan above had to see .bss untouched.
	for( i = 0; i < PSP_BSS_BUCKETS; i++ )
		psp_bssBucketNonZero[ i ] = bucket[ i ];

	psp_bssStart        = (unsigned int)begin;
	psp_bssEnd          = (unsigned int)end;
	psp_bssFirstNonZero = firstNonZero;
	psp_bssNonZeroTotal = total;
}

// Native game modules. The built route is PSP_STATIC_GAME_MODULES: cgame, ui and qagame are
// linked into the EBOOT (a PRX EBOOT with this .bss is refused at load); basegame.cmake explains.

#if defined(PSP_NATIVE_GAME_MODULES) || defined(PSP_STATIC_GAME_MODULES)
// Renamed blob entry points (or PRX import stubs). vmMain* needs vmMainProc's exact 13-int
// signature: varargs and fixed args differ in the MIPS ABI past the fourth argument.
extern void	dllEntryCG( intptr_t ( QDECL *syscallptr )( intptr_t, ... ) );
extern intptr_t	QDECL vmMainCG( int callNum, int arg0, int arg1, int arg2,
			int arg3, int arg4, int arg5, int arg6, int arg7, int arg8,
			int arg9, int arg10, int arg11 );

extern void	dllEntryUI( intptr_t ( QDECL *syscallptr )( intptr_t, ... ) );
extern intptr_t	QDECL vmMainUI( int callNum, int arg0, int arg1, int arg2,
			int arg3, int arg4, int arg5, int arg6, int arg7, int arg8,
			int arg9, int arg10, int arg11 );

#ifdef PSP_STATIC_GAME_MODULES
// qagame is static-only: the PRX build never had stubs for it.
extern void	dllEntryQAG( intptr_t ( QDECL *syscallptr )( intptr_t, ... ) );
extern intptr_t	QDECL vmMainQAG( int callNum, int arg0, int arg1, int arg2,
			int arg3, int arg4, int arg5, int arg6, int arg7, int arg8,
			int arg9, int arg10, int arg11 );
#endif

#ifdef PSP_NATIVE_GAME_MODULES
#define PSP_MAX_GAME_MODULES	3

static SceUID	pspModuleUid[ PSP_MAX_GAME_MODULES ];

// Writes caller locals, never vm_t: an early *entryPoint write survives a failed load and
// makes VM_Call skip the QVM fallback ("User Interface is version -2147352262").
static qboolean PSP_ModuleEntryPoints( const char *name,
	vmMainProc *entryPoint,
	void ( **dllEntry )( intptr_t ( QDECL * )( intptr_t, ... ) ) )
{
	if( !Q_stricmp( name, "cgame" ) )
	{
		*entryPoint = vmMainCG;
		*dllEntry   = dllEntryCG;
		return qtrue;
	}

	if( !Q_stricmp( name, "ui" ) )
	{
		*entryPoint = vmMainUI;
		*dllEntry   = dllEntryUI;
		return qtrue;
	}

	return qfalse;
}
#endif // PSP_NATIVE_GAME_MODULES

// FS_FindVM gives a full path (".../baseq3/cgamemips.prx"); the entry pair is picked by
// module name, so strip the directory, extension and ARCH_STRING suffix.
static void PSP_ModuleBaseName( const char *path, char *out, int outSize )
{
	const char	*slash = strrchr( path, '/' );
	const char	*dot;
	int		len;

	Q_strncpyz( out, slash ? slash + 1 : path, outSize );

	dot = strrchr( out, '.' );
	if( dot )
		out[ dot - out ] = '\0';

	// "cgamemips" -> "cgame"
	len = (int)strlen( out );
	if( len > (int)strlen( ARCH_STRING ) &&
		!Q_stricmp( out + len - strlen( ARCH_STRING ), ARCH_STRING ) )
	{
		out[ len - strlen( ARCH_STRING ) ] = '\0';
	}
}

void *Sys_LoadDll( const char *name, qboolean useSystemLib )
{
	// Only ever called for game modules on this platform, and those go
	// through Sys_LoadGameDll. Never silently succeed.
	Com_Printf( "Sys_LoadDll(%s): not supported on PSP, use Sys_LoadGameDll\n", name );
	return NULL;
}

#ifdef PSP_NATIVE_GAME_MODULES
void *Sys_LoadGameDll( const char *name,
	vmMainProc *entryPoint,
	intptr_t ( *systemcalls )( intptr_t, ... ) )
{
	void		( *dllEntry )( intptr_t ( QDECL * )( intptr_t, ... ) );
	vmMainProc	moduleMain;
	char		base[ MAX_QPATH ];
	SceUID		uid;
	int		slot, status, rc;

	assert( name );

	PSP_ModuleBaseName( name, base, sizeof( base ) );

	// Into locals. *entryPoint is written only once the module is running -
	// see the note on PSP_ModuleEntryPoints.
	if( !PSP_ModuleEntryPoints( base, &moduleMain, &dllEntry ) )
	{
		Com_Printf( "Sys_LoadGameDll(%s): no import stubs linked for module '%s' "
			"(see psp/psp_modules.S)\n", name, base );
		return NULL;
	}

	for( slot = 0; slot < PSP_MAX_GAME_MODULES; slot++ )
	{
		if( !pspModuleUid[ slot ] )
			break;
	}

	if( slot == PSP_MAX_GAME_MODULES )
	{
		Com_Printf( "Sys_LoadGameDll(%s): no free module slot\n", name );
		return NULL;
	}

	Com_Printf( "Sys_LoadGameDll: loading %s\n", name );

	// The user-mode loader refuses an unsigned PRX (0x80020148); kubridge loads it with kernel
	// privileges, as the mirror does. sceKernelLoadModule stays as the fallback for signed builds.
	uid = kuKernelLoadModule( name, 0, NULL );
	if( uid < 0 )
	{
		SceUID	fallback = sceKernelLoadModule( name, 0, NULL );

		if( fallback < 0 )
		{
			Com_Printf( "Sys_LoadGameDll(%s): load failed - "
				"kuKernelLoadModule 0x%08X, sceKernelLoadModule 0x%08X\n",
				name, (unsigned int)uid, (unsigned int)fallback );
			return NULL;
		}

		uid = fallback;
	}

	status = 0;
	rc = sceKernelStartModule( uid, 0, NULL, &status, NULL );
	if( rc < 0 )
	{
		Com_Printf( "Sys_LoadGameDll(%s): sceKernelStartModule failed (0x%08X)\n",
			name, (unsigned int)rc );
		sceKernelUnloadModule( uid );
		return NULL;
	}

	// The stubs are patched during sceKernelStartModule, so only now are the pointers callable;
	// publishing *entryPoint this late keeps the QVM fallback usable on every failure above.
	pspModuleUid[ slot ] = uid;
	*entryPoint          = moduleMain;

	Com_Printf( "Sys_LoadGameDll(%s): started, vmMain %p, %u KB partition free\n",
		base, (void *)moduleMain,
		(unsigned int)( sceKernelMaxFreeMemSize() / 1024 ) );

	dllEntry( systemcalls );

	return (void *)(intptr_t)( slot + 1 );
}

void Sys_UnloadDll( void *dllHandle )
{
	int	slot = (int)(intptr_t)dllHandle - 1;
	int	status, rc;

	if( slot < 0 || slot >= PSP_MAX_GAME_MODULES || !pspModuleUid[ slot ] )
	{
		Com_Printf( "Sys_UnloadDll: bad handle\n" );
		return;
	}

	status = 0;
	rc = sceKernelStopModule( pspModuleUid[ slot ], 0, NULL, &status, NULL );
	if( rc < 0 )
		Com_Printf( "Sys_UnloadDll: sceKernelStopModule failed (0x%08X)\n",
			(unsigned int)rc );

	rc = sceKernelUnloadModule( pspModuleUid[ slot ] );
	if( rc < 0 )
		Com_Printf( "Sys_UnloadDll: sceKernelUnloadModule failed (0x%08X)\n",
			(unsigned int)rc );

	pspModuleUid[ slot ] = 0;

	Com_Printf( "Sys_UnloadDll: slot %d released, %u KB partition free\n",
		slot, (unsigned int)( sceKernelMaxFreeMemSize() / 1024 ) );
}
#endif // PSP_NATIVE_GAME_MODULES

#ifdef PSP_STATIC_GAME_MODULES
// Linked-in modules keep their globals across VM restarts (q3_ui's stale shader handles), so a
// .data snapshot and .bss wipe restore dlopen semantics. basegame.cmake names these sections.
extern char	__start_cgame_data[],  __stop_cgame_data[];
extern char	__start_cgame_bss[],   __stop_cgame_bss[];
extern char	__start_ui_data[],     __stop_ui_data[];
extern char	__start_ui_bss[],      __stop_ui_bss[];
extern char	__start_qagame_data[], __stop_qagame_data[];
extern char	__start_qagame_bss[],  __stop_qagame_bss[];

typedef struct
{
	const char	*name;
	vmMainProc	vmMain;
	void		( *dllEntry )( intptr_t ( QDECL * )( intptr_t, ... ) );
	char		*dataStart, *dataEnd;
	char		*bssStart, *bssEnd;
	void		*pristine;	// .data as linked, taken on first load
	qboolean	active;
} pspStaticModule_t;

static pspStaticModule_t	pspStaticModules[] =
{
	{ "cgame", vmMainCG, dllEntryCG,
		__start_cgame_data, __stop_cgame_data,
		__start_cgame_bss,  __stop_cgame_bss,  NULL, qfalse },
	{ "ui", vmMainUI, dllEntryUI,
		__start_ui_data, __stop_ui_data,
		__start_ui_bss,  __stop_ui_bss,  NULL, qfalse },
	{ "qagame", vmMainQAG, dllEntryQAG,
		__start_qagame_data, __stop_qagame_data,
		__start_qagame_bss,  __stop_qagame_bss,  NULL, qfalse },
};

#define PSP_NUM_STATIC_MODULES \
	( (int)( sizeof( pspStaticModules ) / sizeof( pspStaticModules[ 0 ] ) ) )

// First call: copy the pristine .data. Later calls: restore it and zero .bss. Without a
// snapshot the module still loads, with state persisting, and the log says so.
static void PSP_ModuleResetState( pspStaticModule_t *mod )
{
	size_t	dataLen = (size_t)( mod->dataEnd - mod->dataStart );
	size_t	bssLen  = (size_t)( mod->bssEnd - mod->bssStart );

	if( !mod->pristine )
	{
		mod->pristine = malloc( dataLen );

		if( !mod->pristine )
		{
			Com_Printf( "Sys_LoadGameDll(%s): no memory for the %u byte .data "
				"snapshot - module state will persist across restarts\n",
				mod->name, (unsigned int)dataLen );
			return;
		}

		Com_Memcpy( mod->pristine, mod->dataStart, dataLen );

		Com_Printf( "Sys_LoadGameDll(%s): %u B data snapshot, %u KB bss\n",
			mod->name, (unsigned int)dataLen, (unsigned int)( bssLen / 1024 ) );
		return;
	}

	Com_Memcpy( mod->dataStart, mod->pristine, dataLen );
	Com_Memset( mod->bssStart, 0, bssLen );

	Com_Printf( "Sys_LoadGameDll(%s): state reset (%u B data, %u KB bss)\n",
		mod->name, (unsigned int)dataLen, (unsigned int)( bssLen / 1024 ) );
}

// Static route: reset state, hand back vmMain, run dllEntry. Failing without writing
// *entryPoint is what keeps the pk3's .qvm usable as the fallback.
void *Sys_LoadGameDll( const char *name,
	vmMainProc *entryPoint,
	intptr_t ( *systemcalls )( intptr_t, ... ) )
{
	pspStaticModule_t	*mod = NULL;
	char			base[ MAX_QPATH ];
	int			i;

	assert( name );

	PSP_ModuleBaseName( name, base, sizeof( base ) );

	for( i = 0; i < PSP_NUM_STATIC_MODULES; i++ )
	{
		if( !Q_stricmp( base, pspStaticModules[ i ].name ) )
		{
			mod = &pspStaticModules[ i ];
			break;
		}
	}

	if( !mod )
	{
		// Not an error: qagame has no blob and is meant to stay interpreted.
		Com_Printf( "Sys_LoadGameDll: no built-in module '%s'\n", base );
		return NULL;
	}

	if( mod->active )
	{
		Com_Printf( "Sys_LoadGameDll(%s): already loaded\n", base );
		return NULL;
	}

	// Before dllEntry, which is the module's first executed instruction.
	PSP_ModuleResetState( mod );

	mod->active = qtrue;
	*entryPoint = mod->vmMain;

	Com_Printf( "Sys_LoadGameDll(%s): built in, vmMain %p\n",
		base, (void *)mod->vmMain );

	mod->dllEntry( systemcalls );

	return (void *)(intptr_t)( ( mod - pspStaticModules ) + 1 );
}

// Releases the slot only; PSP_ModuleResetState clears the state on the way back in.
void Sys_UnloadDll( void *dllHandle )
{
	int	slot = (int)(intptr_t)dllHandle - 1;

	if( slot < 0 || slot >= PSP_NUM_STATIC_MODULES ||
		!pspStaticModules[ slot ].active )
	{
		Com_Printf( "Sys_UnloadDll: bad handle\n" );
		return;
	}

	pspStaticModules[ slot ].active = qfalse;
}
#endif // PSP_STATIC_GAME_MODULES

#endif // PSP_NATIVE_GAME_MODULES || PSP_STATIC_GAME_MODULES

// Frame breakdown in microseconds, main thread only. psp_zoneOpen guards the Begin/End pairing,
// so a zone left by an early return adds nothing instead of a stale huge delta.
static unsigned int	psp_zoneStart[ PSP_ZONE_COUNT ];
static int			psp_zoneOpen[ PSP_ZONE_COUNT ];
static unsigned int	psp_zoneUs[ PSP_ZONE_COUNT ];
static unsigned int	psp_zoneMaxUs[ PSP_ZONE_COUNT ];

static unsigned int	psp_frameWindowUs = 0;
static unsigned int	psp_frameCount    = 0;
static unsigned int	psp_frameMaxUs    = 0;
static unsigned int	psp_frameLastUs   = 0;

// 1 ms bins up to 1.024 s: percentiles need no sort at report time, so the profiler cannot
// leak a sorting spike into the next measured frame.
#define PSP_FRAME_HISTOGRAM_BIN_US 1000
#define PSP_FRAME_HISTOGRAM_BINS   1024
static unsigned short psp_frameHistogram[ PSP_FRAME_HISTOGRAM_BINS ];
static unsigned int   psp_frameHistogramOverflow = 0;

unsigned int Sys_PSP_RenderProfileNow( void )
{
	return sceKernelGetSystemTimeLow();
}

#ifdef PSP_RENDER_PROFILE
// One frame in four is timed: at 1/16 a 5 s fight window held only 3 samples. Scopes can
// nest (md3Lerp calls normalize), so their shares are independent, not a partition.
#define PSP_RPROF_SAMPLE_INTERVAL 4

typedef struct {
	unsigned int start;
	unsigned int total;
	unsigned int calls;
} pspRenderProfileScope_t;

// Shader batches in sampled frames, keyed by the shader name pointer; overflow goes to "other".
#define PSP_RPROF_SHADER_SLOTS 48
#define PSP_RPROF_SHADER_TOP   10

typedef struct {
	const char   *key;
	char         name[ MAX_QPATH ];
	unsigned int total;
	unsigned int batches;
	unsigned int verts;
} pspRenderProfileShader_t;

static pspRenderProfileShader_t psp_rprofShader[ PSP_RPROF_SHADER_SLOTS ];
static pspRenderProfileShader_t psp_rprofShaderOther;
static unsigned int psp_rprofShaderCount;

static pspRenderProfileScope_t psp_rprof[ PSP_RPROF_COUNT ];
static unsigned int psp_rprofFrameSerial;
static unsigned int psp_rprofSampledFrames;
static int psp_rprofSampling;

#define PSP_CGAME_SYSCALL_COUNT 128
static pspRenderProfileScope_t psp_cgameSyscall[ PSP_CGAME_SYSCALL_COUNT ];

#define PSP_RPROF_SOUND_ASSET_COUNT 32
static char psp_rprofSoundAsset[ PSP_RPROF_SOUND_ASSET_COUNT ][ MAX_QPATH ];
static unsigned int psp_rprofSoundAssetCount;

static const char * const psp_rprofName[ PSP_RPROF_COUNT ] = {
	"normalize", "md3Lerp", "envTc", "scaleTc", "transformTc", "diffuse",
	"drawScan", "drawPack", "drawIndex", "drawOutcode", "drawClassify",
	"drawClip", "drawSubmit", "fastTc", "world", "deform", "surfTri",
	"surfFace", "surfGrid", "colors", "dlight", "fog", "clientFrame",
	"sound", "soundPaint", "cgameSim", "cgameEnts", "cgameWeapon",
	"cgameDraw", "backendCmds", "backendSurfs", "cgameSnapshot",
	"cgamePredict", "cgameView", "cgamePacket", "cgameMarks",
	"cgameParticles", "cgameLocal", "cgameScene", "cgameHud",
	"soundLazyLoad", "soundEvict", "soundCodec", "soundResample",
	"soundCodecOpen", "soundCodecHeader", "soundCodecAlloc",
	"soundCodecRead", "soundPcmAlloc", "soundPoolAlloc"
};
#endif

// Counters cover every operation; only events of 1 ms or more enter the slow list.
// Categories overlap (a restore-open inside its restore), so totals are not additive.
#define PSP_FILE_TRACE_SLOW_US 1000
#define PSP_FILE_TRACE_TOP     8

#ifdef PSP_FILE_TRACE
typedef struct {
	unsigned int category;
	unsigned int elapsedUs;
	unsigned int frameSerial;
	unsigned int value;
} pspFileTraceEvent_t;

static unsigned int psp_fileTraceFrameSerial;
static unsigned int psp_fileTraceCount[ PSP_TRACE_COUNT ];
static unsigned int psp_fileTraceTotalUs[ PSP_TRACE_COUNT ];
static unsigned int psp_fileTraceMaxUs[ PSP_TRACE_COUNT ];
static pspFileTraceEvent_t psp_fileTraceSlow[ PSP_FILE_TRACE_TOP ];
static unsigned int psp_fileTraceSlowCount;

static const char * const psp_fileTraceName[ PSP_TRACE_COUNT ] = {
	"vfOpen", "vfRead", "vfSeek", "evict", "restore", "restoreOpen",
	"restoreSeek", "soundLazyLoad", "soundCodec", "soundCodecOpen",
	"soundCodecHeader", "soundCodecAlloc", "soundCodecRead",
	"soundPcmAlloc", "soundResample", "netSelect", "netDelay"
};
#endif

void Sys_PSP_FileTraceFrameBegin( void )
{
#ifdef PSP_FILE_TRACE
	psp_fileTraceFrameSerial++;
#endif
}

void Sys_PSP_FileTraceEvent( int category, unsigned int elapsedUs,
	unsigned int value )
{
#ifdef PSP_FILE_TRACE
	unsigned int i;
	unsigned int slot;

	if( category < 0 || category >= PSP_TRACE_COUNT ) {
		return;
	}

	psp_fileTraceCount[ category ]++;
	psp_fileTraceTotalUs[ category ] += elapsedUs;
	if( elapsedUs > psp_fileTraceMaxUs[ category ] ) {
		psp_fileTraceMaxUs[ category ] = elapsedUs;
	}

	if( elapsedUs < PSP_FILE_TRACE_SLOW_US ) {
		return;
	}

	if( psp_fileTraceSlowCount < PSP_FILE_TRACE_TOP ) {
		slot = psp_fileTraceSlowCount++;
	} else {
		slot = 0;
		for( i = 1; i < PSP_FILE_TRACE_TOP; i++ ) {
			if( psp_fileTraceSlow[ i ].elapsedUs <
				psp_fileTraceSlow[ slot ].elapsedUs ) {
				slot = i;
			}
		}
		if( elapsedUs <= psp_fileTraceSlow[ slot ].elapsedUs ) {
			return;
		}
	}

	psp_fileTraceSlow[ slot ].category    = (unsigned int)category;
	psp_fileTraceSlow[ slot ].elapsedUs   = elapsedUs;
	psp_fileTraceSlow[ slot ].frameSerial = psp_fileTraceFrameSerial;
	psp_fileTraceSlow[ slot ].value       = value;
#else
	(void)category;
	(void)elapsedUs;
	(void)value;
#endif
}

static void Sys_PSP_FileTraceReport( void )
{
#ifdef PSP_FILE_TRACE
	unsigned int i;

	Com_Printf( "PSP file trace: categories overlap; totals are not additive\n" );
	for( i = 0; i < PSP_TRACE_COUNT; i++ ) {
		Com_Printf( "PSP file trace:   %-16s count %u total %u.%03u ms max %u.%03u ms\n",
			psp_fileTraceName[ i ], psp_fileTraceCount[ i ],
			psp_fileTraceTotalUs[ i ] / 1000,
			psp_fileTraceTotalUs[ i ] % 1000,
			psp_fileTraceMaxUs[ i ] / 1000,
			psp_fileTraceMaxUs[ i ] % 1000 );
	}

	/* Sort only the eight retained records, outside the measured frame path. */
	for( i = 0; i < psp_fileTraceSlowCount; i++ ) {
		unsigned int j;
		unsigned int best = i;

		for( j = i + 1; j < psp_fileTraceSlowCount; j++ ) {
			if( psp_fileTraceSlow[ j ].elapsedUs >
				psp_fileTraceSlow[ best ].elapsedUs ) {
				best = j;
			}
		}
		if( best != i ) {
			pspFileTraceEvent_t event = psp_fileTraceSlow[ i ];
			psp_fileTraceSlow[ i ] = psp_fileTraceSlow[ best ];
			psp_fileTraceSlow[ best ] = event;
		}
	}

	for( i = 0; i < psp_fileTraceSlowCount; i++ ) {
		const pspFileTraceEvent_t *event = &psp_fileTraceSlow[ i ];

		Com_Printf( "PSP file trace slow: %-16s elapsed %u us frame %u value %u\n",
			psp_fileTraceName[ event->category ], event->elapsedUs,
			event->frameSerial, event->value );
	}

	memset( psp_fileTraceCount, 0, sizeof( psp_fileTraceCount ) );
	memset( psp_fileTraceTotalUs, 0, sizeof( psp_fileTraceTotalUs ) );
	memset( psp_fileTraceMaxUs, 0, sizeof( psp_fileTraceMaxUs ) );
	psp_fileTraceSlowCount = 0;
#endif
}

#ifdef PSP_STUTTER_TRACE
char *Sys_PSP_BasePath( void );

// Goal 23 lookup trace: slow complete operations plus per-phase/per-frame counters. Nothing is
// written to storage until CON_Shutdown calls Sys_PSP_StutterTraceDump, after the workload.
#define PSP_STUTTER_SLOW_US       1000
#define PSP_STUTTER_SLOW_RECORDS  2048
#define PSP_STUTTER_FRAME_SLOTS   512
#define PSP_STUTTER_QPATHS        256
#define PSP_STUTTER_PACKS         64

typedef struct {
	unsigned int timestampUs;
	unsigned int frameSerial;
	unsigned int qpathHash;
	unsigned int packHash;
	unsigned int phase;
	unsigned int source;
	unsigned int elapsedUs;
	int logicalHandle;
	int physicalSlot;
	int offset;
	int origin;
	int savedCursor;
	int result;
	unsigned int flags;
} pspStutterTraceRecord_t;

typedef struct {
	unsigned int frameSerial;
	unsigned int count[ PSP_STUTTER_PHASE_COUNT ];
	unsigned int totalUs[ PSP_STUTTER_PHASE_COUNT ];
	unsigned int maxUs[ PSP_STUTTER_PHASE_COUNT ];
} pspStutterFrameSummary_t;

typedef struct {
	unsigned int hash;
	char path[ MAX_QPATH ];
} pspStutterQpathEntry_t;

typedef struct {
	unsigned int hash;
	char path[ MAX_OSPATH ];
} pspStutterPackEntry_t;

typedef struct {
	char magic[ 8 ];
	unsigned int version;
	char buildId[ 64 ];
	unsigned int frameSerial;
	unsigned int phaseCount;
	unsigned int qpathCount;
	unsigned int packCount;
	unsigned int frameSlotCount;
	unsigned int slowRecordCount;
	unsigned int slowRecordStart;
	unsigned int slowRecordOverflow;
	unsigned int qpathOverflow;
	unsigned int packOverflow;
} pspStutterTraceHeader_t;

static unsigned int psp_stutterCount[ PSP_STUTTER_PHASE_COUNT ];
static unsigned int psp_stutterTotalUs[ PSP_STUTTER_PHASE_COUNT ];
static unsigned int psp_stutterMaxUs[ PSP_STUTTER_PHASE_COUNT ];
static pspStutterFrameSummary_t psp_stutterFrames[ PSP_STUTTER_FRAME_SLOTS ];
static pspStutterTraceRecord_t psp_stutterSlow[ PSP_STUTTER_SLOW_RECORDS ];
static unsigned int psp_stutterSlowWrite;
static unsigned int psp_stutterSlowCount;
static unsigned int psp_stutterSlowOverflow;
static pspStutterQpathEntry_t psp_stutterQpaths[ PSP_STUTTER_QPATHS ];
static pspStutterPackEntry_t psp_stutterPacks[ PSP_STUTTER_PACKS ];
static unsigned int psp_stutterQpathCount;
static unsigned int psp_stutterPackCount;
static unsigned int psp_stutterQpathOverflow;
static unsigned int psp_stutterPackOverflow;
static int psp_stutterContextActive;
static int psp_stutterLookupActive;
static unsigned int psp_stutterQpathHash;
static unsigned int psp_stutterPackHash;
static unsigned int psp_stutterLookupStart;
static int psp_stutterLookupSource;
static int psp_stutterPhase;
static int psp_stutterDumped;

static const char psp_stutterPhaseName[ PSP_STUTTER_PHASE_COUNT ][ 16 ] = {
	"none", "lookupTotal", "looseStat", "looseFopen", "packHash",
	"unzOffset", "localHeader", "refillSeek", "refillRead", "vfOpen",
	"vfRead", "vfSeek", "vfEvict", "vfRestore", "restoreOpen",
	"restoreSeek"
};

unsigned int Sys_PSP_StutterTraceHashPath( const char *path )
{
	unsigned int hash = 2166136261u;
	const unsigned char *p = (const unsigned char *)path;

	if( !p )
		return 0;

	while( *p )
	{
		unsigned char c = *p++;

		if( c == '\\' )
			c = '/';
		if( c >= 'A' && c <= 'Z' )
			c = (unsigned char)( c + ( 'a' - 'A' ) );

		hash ^= c;
		hash *= 16777619u;
	}

	return hash;
}

static void Sys_PSP_StutterRememberQpath( const char *path,
	unsigned int hash )
{
	unsigned int i;

	if( !path || !*path || !hash )
		return;

	for( i = 0; i < psp_stutterQpathCount; i++ )
	{
		if( psp_stutterQpaths[ i ].hash == hash )
			return;
	}

	if( psp_stutterQpathCount >= PSP_STUTTER_QPATHS )
	{
		psp_stutterQpathOverflow++;
		return;
	}

	psp_stutterQpaths[ psp_stutterQpathCount ].hash = hash;
	Q_strncpyz( psp_stutterQpaths[ psp_stutterQpathCount ].path,
		path, sizeof( psp_stutterQpaths[ psp_stutterQpathCount ].path ) );
	psp_stutterQpathCount++;
}

static void Sys_PSP_StutterRememberPack( const char *path,
	unsigned int hash )
{
	unsigned int i;

	if( !path || !*path || !hash )
		return;

	for( i = 0; i < psp_stutterPackCount; i++ )
	{
		if( psp_stutterPacks[ i ].hash == hash )
			return;
	}

	if( psp_stutterPackCount >= PSP_STUTTER_PACKS )
	{
		psp_stutterPackOverflow++;
		return;
	}

	psp_stutterPacks[ psp_stutterPackCount ].hash = hash;
	Q_strncpyz( psp_stutterPacks[ psp_stutterPackCount ].path,
		path, sizeof( psp_stutterPacks[ psp_stutterPackCount ].path ) );
	psp_stutterPackCount++;
}

static void Sys_PSP_StutterFrameEvent( unsigned int frameSerial,
	int phase, unsigned int elapsedUs )
{
	pspStutterFrameSummary_t *frame;

	frame = &psp_stutterFrames[ frameSerial % PSP_STUTTER_FRAME_SLOTS ];
	if( frame->frameSerial != frameSerial )
	{
		memset( frame, 0, sizeof( *frame ) );
		frame->frameSerial = frameSerial;
	}

	frame->count[ phase ]++;
	frame->totalUs[ phase ] += elapsedUs;
	if( elapsedUs > frame->maxUs[ phase ] )
		frame->maxUs[ phase ] = elapsedUs;
}

void Sys_PSP_StutterTraceRecord( int phase, unsigned int elapsedUs,
	int logicalHandle, int physicalSlot, int offset, int origin,
	int savedCursor, int result, unsigned int flags, const char *packPath )
{
	pspStutterTraceRecord_t *record;
	unsigned int hash;

	if( phase < 0 || phase >= PSP_STUTTER_PHASE_COUNT )
		return;

	hash = Sys_PSP_StutterTraceHashPath( packPath );
	if( packPath && *packPath )
	{
		Sys_PSP_StutterRememberPack( packPath, hash );
		if( !psp_stutterPackHash )
			psp_stutterPackHash = hash;
	}

	psp_stutterCount[ phase ]++;
	psp_stutterTotalUs[ phase ] += elapsedUs;
	if( elapsedUs > psp_stutterMaxUs[ phase ] )
		psp_stutterMaxUs[ phase ] = elapsedUs;
	Sys_PSP_StutterFrameEvent( psp_fileTraceFrameSerial, phase, elapsedUs );

	if( elapsedUs < PSP_STUTTER_SLOW_US )
		return;

	record = &psp_stutterSlow[ psp_stutterSlowWrite % PSP_STUTTER_SLOW_RECORDS ];
	memset( record, 0, sizeof( *record ) );
	record->timestampUs   = sceKernelGetSystemTimeLow();
	record->frameSerial   = psp_fileTraceFrameSerial;
	record->qpathHash     = psp_stutterQpathHash;
	record->packHash      = psp_stutterPackHash ? psp_stutterPackHash : hash;
	record->phase         = (unsigned int)phase;
	record->source        = (unsigned int)psp_stutterLookupSource;
	record->elapsedUs     = elapsedUs;
	record->logicalHandle = logicalHandle;
	record->physicalSlot  = physicalSlot;
	record->offset        = offset;
	record->origin        = origin;
	record->savedCursor   = savedCursor;
	record->result        = result;
	record->flags         = flags;

	psp_stutterSlowWrite++;
	if( psp_stutterSlowCount < PSP_STUTTER_SLOW_RECORDS )
		psp_stutterSlowCount++;
	else
		psp_stutterSlowOverflow++;
}

void Sys_PSP_StutterTraceLookupBegin( const char *qpath )
{
	psp_stutterLookupActive = 1;
	psp_stutterContextActive = 1;
	psp_stutterQpathHash = Sys_PSP_StutterTraceHashPath( qpath );
	psp_stutterPackHash = 0;
	psp_stutterLookupSource = PSP_STUTTER_SOURCE_MISS;
	psp_stutterLookupStart = sceKernelGetSystemTimeLow();
	psp_stutterPhase = PSP_STUTTER_PHASE_NONE;
	Sys_PSP_StutterRememberQpath( qpath, psp_stutterQpathHash );
}

void Sys_PSP_StutterTraceLookupSource( int source )
{
	if( source >= PSP_STUTTER_SOURCE_MISS &&
		source <= PSP_STUTTER_SOURCE_PACK )
	{
		psp_stutterLookupSource = source;
		if( source != PSP_STUTTER_SOURCE_PACK )
			psp_stutterPackHash = 0;
	}
}

void Sys_PSP_StutterTraceLookupEnd( int result )
{
	unsigned int elapsedUs;

	if( !psp_stutterLookupActive )
		return;

	elapsedUs = sceKernelGetSystemTimeLow() - psp_stutterLookupStart;
	if( psp_stutterLookupSource != PSP_STUTTER_SOURCE_PACK )
		psp_stutterPackHash = 0;
	Sys_PSP_StutterTraceRecord( PSP_STUTTER_PHASE_LOOKUP_TOTAL,
		elapsedUs, -1, -1, 0, 0, 0, result, PSP_STUTTER_FLAG_PARENT, NULL );
	psp_stutterLookupActive = 0;
	psp_stutterContextActive = 0;
	psp_stutterQpathHash = 0;
	psp_stutterPackHash = 0;
	psp_stutterLookupSource = PSP_STUTTER_SOURCE_MISS;
	psp_stutterPhase = PSP_STUTTER_PHASE_NONE;
}

void Sys_PSP_StutterTraceSetContext( const char *qpath,
	unsigned int packHash )
{
	psp_stutterContextActive = 1;
	if( qpath )
	{
		psp_stutterQpathHash = Sys_PSP_StutterTraceHashPath( qpath );
		Sys_PSP_StutterRememberQpath( qpath, psp_stutterQpathHash );
	}
	psp_stutterPackHash = packHash;
}

void Sys_PSP_StutterTraceClearContext( void )
{
	if( !psp_stutterLookupActive )
	{
		psp_stutterContextActive = 0;
		psp_stutterQpathHash = 0;
		psp_stutterPackHash = 0;
		psp_stutterPhase = PSP_STUTTER_PHASE_NONE;
	}
}

void Sys_PSP_StutterTraceSetPack( const char *packPath )
{
	psp_stutterPackHash = Sys_PSP_StutterTraceHashPath( packPath );
	Sys_PSP_StutterRememberPack( packPath, psp_stutterPackHash );
}

void Sys_PSP_StutterTraceSetPhase( int phase )
{
	psp_stutterPhase = phase;
}

int Sys_PSP_StutterTraceCurrentPhase( int fallback )
{
	return psp_stutterPhase != PSP_STUTTER_PHASE_NONE ?
		psp_stutterPhase : fallback;
}

static int Sys_PSP_StutterWrite( SceUID fd, const void *data, int size )
{
	return sceIoWrite( fd, data, size ) == size;
}

void Sys_PSP_StutterTraceDump( void )
{
	pspStutterTraceHeader_t header;
	char path[ MAX_OSPATH ];
	SceUID fd;
	unsigned int i;
	unsigned int recordCount;
	unsigned int recordStart;

	if( psp_stutterDumped )
		return;
	psp_stutterDumped = 1;

	Com_sprintf( path, sizeof( path ), "%s/q3psp%d.trace",
		Sys_PSP_BasePath(), PSP_LOG_GEN );
	fd = sceIoOpen( path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777 );
	if( fd < 0 )
		return;

	memset( &header, 0, sizeof( header ) );
	memcpy( header.magic, "Q3STRC1", 7 );
	header.version = 2;
	Q_strncpyz( header.buildId, PSP_PERF_BUILD_ID, sizeof( header.buildId ) );
	header.frameSerial = psp_fileTraceFrameSerial;
	header.phaseCount = PSP_STUTTER_PHASE_COUNT;
	header.qpathCount = psp_stutterQpathCount;
	header.packCount = psp_stutterPackCount;
	header.frameSlotCount = PSP_STUTTER_FRAME_SLOTS;
	header.slowRecordCount = psp_stutterSlowCount < PSP_STUTTER_SLOW_RECORDS ?
		psp_stutterSlowCount : PSP_STUTTER_SLOW_RECORDS;
	header.slowRecordStart = psp_stutterSlowCount > PSP_STUTTER_SLOW_RECORDS ?
		psp_stutterSlowWrite % PSP_STUTTER_SLOW_RECORDS : 0;
	header.slowRecordOverflow = psp_stutterSlowOverflow;
	header.qpathOverflow = psp_stutterQpathOverflow;
	header.packOverflow = psp_stutterPackOverflow;

	if( !Sys_PSP_StutterWrite( fd, &header, sizeof( header ) ) ||
		!Sys_PSP_StutterWrite( fd, psp_stutterPhaseName,
			sizeof( psp_stutterPhaseName ) ) ||
		!Sys_PSP_StutterWrite( fd, psp_stutterCount,
			(int)sizeof( psp_stutterCount ) ) ||
		!Sys_PSP_StutterWrite( fd, psp_stutterTotalUs,
			(int)sizeof( psp_stutterTotalUs ) ) ||
		!Sys_PSP_StutterWrite( fd, psp_stutterMaxUs,
			(int)sizeof( psp_stutterMaxUs ) ) ||
		!Sys_PSP_StutterWrite( fd, psp_stutterQpaths,
			(int)sizeof( psp_stutterQpaths ) ) ||
		!Sys_PSP_StutterWrite( fd, psp_stutterPacks,
			(int)sizeof( psp_stutterPacks ) ) ||
		!Sys_PSP_StutterWrite( fd, psp_stutterFrames,
			(int)sizeof( psp_stutterFrames ) ) )
	{
		sceIoClose( fd );
		return;
	}

	recordCount = psp_stutterSlowCount > PSP_STUTTER_SLOW_RECORDS ?
		PSP_STUTTER_SLOW_RECORDS : psp_stutterSlowCount;
	recordStart = header.slowRecordStart;
	for( i = 0; i < recordCount; i++ )
	{
		unsigned int slot = ( recordStart + i ) % PSP_STUTTER_SLOW_RECORDS;
		if( !Sys_PSP_StutterWrite( fd, &psp_stutterSlow[ slot ],
			(int)sizeof( psp_stutterSlow[ slot ] ) ) )
			break;
	}

	sceIoClose( fd );
}
#endif

#ifdef PSP_RENDER_PROFILE
void Sys_PSP_RenderProfileFrameBegin( void )
{
	psp_rprofSampling = ( ( psp_rprofFrameSerial++ % PSP_RPROF_SAMPLE_INTERVAL ) == 0 );
	if( psp_rprofSampling ) {
		psp_rprofSampledFrames++;
	}
}

void Sys_PSP_RenderProfileBegin( int scope )
{
	if( !psp_rprofSampling || scope < 0 || scope >= PSP_RPROF_COUNT ) {
		return;
	}

	psp_rprof[ scope ].start = sceKernelGetSystemTimeLow();
}

void Sys_PSP_RenderProfileCount( int scope )
{
	if( !psp_rprofSampling || scope < 0 || scope >= PSP_RPROF_COUNT ) {
		return;
	}

	psp_rprof[ scope ].calls++;
}

void Sys_PSP_RenderProfileEnd( int scope )
{
	unsigned int elapsed;

	if( !psp_rprofSampling || scope < 0 || scope >= PSP_RPROF_COUNT ) {
		return;
	}

	elapsed = sceKernelGetSystemTimeLow() - psp_rprof[ scope ].start;
	psp_rprof[ scope ].total += elapsed;
	psp_rprof[ scope ].calls++;
}

void Sys_PSP_RenderProfileSoundAsset( const char *name )
{
	unsigned int i;

	if( !psp_rprofSampling || !name || !name[ 0 ] ) {
		return;
	}

	for( i = 0; i < psp_rprofSoundAssetCount; i++ ) {
		if( !Q_stricmp( psp_rprofSoundAsset[ i ], name ) ) {
			return;
		}
	}

	if( psp_rprofSoundAssetCount >= PSP_RPROF_SOUND_ASSET_COUNT ) {
		return;
	}

	Q_strncpyz( psp_rprofSoundAsset[ psp_rprofSoundAssetCount ], name,
		MAX_QPATH );
	psp_rprofSoundAssetCount++;
}

void Sys_PSP_RenderProfileSoundLoad( const char *name, int rate, int width,
	int channels, int bytes, int loadCount )
{
	if( !name || !name[ 0 ] ) {
		return;
	}

#ifndef PSP_FILE_TRACE
	Com_Printf( "PSP sound load: %s load#%d %dHz %dch %dbit %d bytes %s\n",
		name, loadCount, rate, channels, width * 8, bytes,
		loadCount > 1 ? "reload" : "first" );
#else
	(void)rate;
	(void)width;
	(void)channels;
	(void)bytes;
	(void)loadCount;
#endif
}

unsigned int Sys_PSP_RenderProfileCGameSyscallBegin( int callNum )
{
	if( !psp_rprofSampling || callNum < 0 ||
		callNum >= PSP_CGAME_SYSCALL_COUNT ) {
		return 0;
	}

	return sceKernelGetSystemTimeLow();
}

void Sys_PSP_RenderProfileCGameSyscallEnd( int callNum, unsigned int start )
{
	unsigned int elapsed;

	if( !start || !psp_rprofSampling || callNum < 0 ||
		callNum >= PSP_CGAME_SYSCALL_COUNT ) {
		return;
	}

	elapsed = sceKernelGetSystemTimeLow() - start;
	psp_cgameSyscall[ callNum ].total += elapsed;
	psp_cgameSyscall[ callNum ].calls++;
}

unsigned int Sys_PSP_RenderProfileBatchBegin( void )
{
	return psp_rprofSampling ? sceKernelGetSystemTimeLow() : 0;
}

void Sys_PSP_RenderProfileBatchEnd( unsigned int start, const char *shader, int numVertexes )
{
	pspRenderProfileShader_t *slot = NULL;
	unsigned int i;

	if( !start || !psp_rprofSampling || !shader ) {
		return;
	}

	for( i = 0; i < psp_rprofShaderCount; i++ ) {
		if( psp_rprofShader[ i ].key == shader ) {
			slot = &psp_rprofShader[ i ];
			break;
		}
	}

	if( !slot ) {
		if( psp_rprofShaderCount < PSP_RPROF_SHADER_SLOTS ) {
			slot = &psp_rprofShader[ psp_rprofShaderCount++ ];
			slot->key = shader;
			Q_strncpyz( slot->name, shader, sizeof( slot->name ) );
		} else {
			slot = &psp_rprofShaderOther;
		}
	}

	slot->total += sceKernelGetSystemTimeLow() - start;
	slot->batches++;
	slot->verts += numVertexes;
}

// Top batches by time; each includes surface tessellation and the stage draws.
static void Sys_PSP_RenderProfileShaderReport( void )
{
	unsigned int n, i;

	for( n = 0; n < PSP_RPROF_SHADER_TOP; n++ ) {
		pspRenderProfileShader_t *best = NULL;

		for( i = 0; i < psp_rprofShaderCount; i++ ) {
			if( psp_rprofShader[ i ].batches &&
				( !best || psp_rprofShader[ i ].total > best->total ) ) {
				best = &psp_rprofShader[ i ];
			}
		}
		if( !best ) {
			break;
		}

		Com_Printf( "PSP rprof:   shader %u us/sample-frame, %u batches, %u verts: %s\n",
			best->total / psp_rprofSampledFrames, best->batches, best->verts, best->name );
		best->batches = 0;
	}

	if( psp_rprofShaderOther.batches ) {
		Com_Printf( "PSP rprof:   shader %u us/sample-frame, %u batches, %u verts: (table full)\n",
			psp_rprofShaderOther.total / psp_rprofSampledFrames,
			psp_rprofShaderOther.batches, psp_rprofShaderOther.verts );
	}

	memset( psp_rprofShader, 0, sizeof( psp_rprofShader ) );
	memset( &psp_rprofShaderOther, 0, sizeof( psp_rprofShaderOther ) );
	psp_rprofShaderCount = 0;
}

static void Sys_PSP_RenderProfileReport( unsigned int windowUs, unsigned int frameCount )
{
	unsigned int i;
	unsigned int meanFrameUs;

	if( !psp_rprofSampledFrames || !frameCount ) {
		memset( psp_rprof, 0, sizeof( psp_rprof ) );
		memset( psp_cgameSyscall, 0, sizeof( psp_cgameSyscall ) );
		memset( psp_rprofShader, 0, sizeof( psp_rprofShader ) );
		memset( &psp_rprofShaderOther, 0, sizeof( psp_rprofShaderOther ) );
		psp_rprofShaderCount = 0;
		psp_rprofSoundAssetCount = 0;
		psp_rprofSampledFrames = 0;
		return;
	}

	meanFrameUs = windowUs / frameCount;
	Com_Printf( "PSP rprof: %u sampled renderer frames (1/%u); scopes overlap, do not sum shares\n",
		psp_rprofSampledFrames, PSP_RPROF_SAMPLE_INTERVAL );

	for( i = 0; i < PSP_RPROF_COUNT; i++ ) {
		const pspRenderProfileScope_t *scope = &psp_rprof[ i ];
		unsigned int perSampleFrame;

		if( !scope->calls ) {
			continue;
		}

		if( i == PSP_RPROF_DRAW_SCAN || i == PSP_RPROF_DRAW_SUBMIT ||
			i == PSP_RPROF_FASTTC || i == PSP_RPROF_SURF_FACE ) {
			Com_Printf( "PSP rprof:   %-12s %u calls (untimed, inside backendSurfs)\n",
				psp_rprofName[ i ], scope->calls );
			continue;
		}

		perSampleFrame = scope->total / psp_rprofSampledFrames;
		Com_Printf( "PSP rprof:   %-12s %u us/sample-frame, %u us/call, share %u%% (%u calls)\n",
			psp_rprofName[ i ], perSampleFrame, scope->total / scope->calls,
			meanFrameUs ? ( perSampleFrame * 100 ) / meanFrameUs : 0,
			scope->calls );
	}

	// soundPaint is a strict subset of sound, so the rest is derived without timestamping
	// every non-paint fragment of S_Update.
	if( psp_rprof[ PSP_RPROF_SOUND_UPDATE ].calls &&
		psp_rprof[ PSP_RPROF_SOUND_UPDATE ].total >=
			psp_rprof[ PSP_RPROF_SOUND_PAINT ].total ) {
		const pspRenderProfileScope_t *sound =
			&psp_rprof[ PSP_RPROF_SOUND_UPDATE ];
		const unsigned int restTotal = sound->total -
			psp_rprof[ PSP_RPROF_SOUND_PAINT ].total;
		const unsigned int restPerSample = restTotal / psp_rprofSampledFrames;

		Com_Printf( "PSP rprof:   %-12s %u us/sample-frame, %u us/call, share %u%% (%u calls)\n",
			"soundRest", restPerSample, restTotal / sound->calls,
			meanFrameUs ? ( restPerSample * 100 ) / meanFrameUs : 0,
			sound->calls );
	}

	for( i = 0; i < PSP_CGAME_SYSCALL_COUNT; i++ ) {
		const pspRenderProfileScope_t *call = &psp_cgameSyscall[ i ];

		if( !call->calls ) {
			continue;
		}

		Com_Printf( "PSP cgame syscall: %d %u us/sample-frame, %u us/call, %u calls\n",
			i, call->total / psp_rprofSampledFrames,
			call->total / call->calls, call->calls );
	}

	for( i = 0; i < psp_rprofSoundAssetCount; i++ ) {
		Com_Printf( "PSP sound asset sampled: %s\n", psp_rprofSoundAsset[ i ] );
	}

	Sys_PSP_RenderProfileShaderReport();

	memset( psp_rprof, 0, sizeof( psp_rprof ) );
	memset( psp_cgameSyscall, 0, sizeof( psp_cgameSyscall ) );
	psp_rprofSoundAssetCount = 0;
	psp_rprofSampledFrames = 0;
}
#endif

#ifdef PSP_COUNTERS
typedef struct {
	unsigned int calls;
	unsigned int totalUs;
	unsigned int maxUs;
	unsigned int value;
} pspCounter_t;

#define PSP_COUNT_MISS_NAMES 48
#define PSP_COUNT_MISS_EXTS  12

static pspCounter_t psp_count[ PSP_COUNT_COUNT ];
static char         psp_countMissName[ PSP_COUNT_MISS_NAMES ][ MAX_QPATH ];
static unsigned int psp_countMissNames;
static unsigned int psp_countMissDropped;
static char         psp_countMissExt[ PSP_COUNT_MISS_EXTS ][ 8 ];
static unsigned int psp_countMissExtCalls[ PSP_COUNT_MISS_EXTS ];
static unsigned int psp_countMissExts;

static const char * const psp_countName[ PSP_COUNT_COUNT ] = {
	"fsLookup", "fsMiss", "fsLoose", "fsLooseSkip", "fsPackOpen", "fsUnzOpen", "fsRead",
	"imageLoad", "imageCreate", "dxt", "world",
	"svBots", "svGame", "svSnap", "svTrace",
	"aasAreaCache", "aasPortalCache", "aasFreeMem", "aasFreeCap",
	"botChar", "botChat", "botWeight", "aasBudget"
};

// botlib's routing cache bytes and its PSP cap (be_aas_route.c).
extern int routingcachesize, max_routingcachesize;

unsigned int Sys_PSP_CountBegin( void )
{
	return sceKernelGetSystemTimeLow();
}

void Sys_PSP_CountEnd( int counter, unsigned int start, unsigned int value )
{
	unsigned int us;

	if( counter < 0 || counter >= PSP_COUNT_COUNT )
		return;

	us = sceKernelGetSystemTimeLow() - start;
	psp_count[ counter ].calls++;
	psp_count[ counter ].totalUs += us;
	psp_count[ counter ].value += value;
	if( us > psp_count[ counter ].maxUs )
		psp_count[ counter ].maxUs = us;
}

void Sys_PSP_CountEvent( int counter )
{
	if( counter >= 0 && counter < PSP_COUNT_COUNT )
		psp_count[ counter ].calls++;
}

void Sys_PSP_CountMiss( const char *qpath )
{
	const char *ext;
	unsigned int i;

	if( !qpath || !*qpath )
		return;

	ext = COM_GetExtension( qpath );
	for( i = 0; i < psp_countMissExts; i++ ) {
		if( !Q_stricmp( psp_countMissExt[ i ], ext ) )
			break;
	}
	if( i == psp_countMissExts && psp_countMissExts < PSP_COUNT_MISS_EXTS )
		Q_strncpyz( psp_countMissExt[ psp_countMissExts++ ], ext, sizeof( psp_countMissExt[ 0 ] ) );
	if( i < psp_countMissExts )
		psp_countMissExtCalls[ i ]++;

	for( i = 0; i < psp_countMissNames; i++ ) {
		if( !Q_stricmp( psp_countMissName[ i ], qpath ) )
			return;
	}
	if( psp_countMissNames < PSP_COUNT_MISS_NAMES )
		Q_strncpyz( psp_countMissName[ psp_countMissNames++ ], qpath, MAX_QPATH );
	else
		psp_countMissDropped++;
}

void Sys_PSP_CountReport( const char *where, unsigned int frameCount )
{
	char         line[ 256 ];
	unsigned int i;

	for( i = 0; i < PSP_COUNT_COUNT; i++ ) {
		const pspCounter_t *c = &psp_count[ i ];

		if( !c->calls )
			continue;

		Com_sprintf( line, sizeof( line ), "PSP count [%s]: %-14s %u calls, %u ms, max %u us",
			where, psp_countName[ i ], c->calls, c->totalUs / 1000, c->maxUs );
		if( c->value )
			Q_strcat( line, sizeof( line ), va( ", value %u", c->value ) );
		if( frameCount )
			Q_strcat( line, sizeof( line ), va( ", %u us/frame", c->totalUs / frameCount ) );
		Com_Printf( "%s\n", line );
	}

	// aasFreeMem fires when zone free drops under 1 MB, aasFreeCap at max_routingcache.
	if( psp_count[ PSP_COUNT_SV_BOTS ].calls ) {
		Com_Printf( "PSP count [%s]: routing cache %d KB of %d KB cap, zone %d KB free\n",
			where, routingcachesize / 1024, max_routingcachesize / 1024,
			Z_AvailableMemory() / 1024 );
	}

	if( psp_countMissExts ) {
		Com_sprintf( line, sizeof( line ), "PSP count [%s]: misses by extension:", where );
		for( i = 0; i < psp_countMissExts; i++ ) {
			Q_strcat( line, sizeof( line ), va( " %s %u", psp_countMissExt[ i ][ 0 ] ?
				psp_countMissExt[ i ] : "(none)", psp_countMissExtCalls[ i ] ) );
		}
		Com_Printf( "%s\n", line );
	}
	for( i = 0; i < psp_countMissNames; i++ ) {
		Com_Printf( "PSP miss: %s\n", psp_countMissName[ i ] );
	}
	if( psp_countMissDropped ) {
		Com_Printf( "PSP miss: %u more misses not listed\n", psp_countMissDropped );
	}

	memset( psp_count, 0, sizeof( psp_count ) );
	memset( psp_countMissExtCalls, 0, sizeof( psp_countMissExtCalls ) );
	psp_countMissNames = 0;
	psp_countMissDropped = 0;
	psp_countMissExts = 0;
}
#endif

static unsigned int Sys_PSP_FramePercentileUs( unsigned int percent )
{
	const unsigned int rank = ( psp_frameCount * percent + 99 ) / 100;
	unsigned int cumulative = 0;
	unsigned int i;

	for( i = 0; i < PSP_FRAME_HISTOGRAM_BINS; i++ ) {
		cumulative += psp_frameHistogram[ i ];
		if( cumulative >= rank ) {
			return i * PSP_FRAME_HISTOGRAM_BIN_US;
		}
	}

	/* Saturated samples are reported at the histogram's upper boundary. */
	return PSP_FRAME_HISTOGRAM_BINS * PSP_FRAME_HISTOGRAM_BIN_US;
}

static const char * const psp_zoneName[ PSP_ZONE_COUNT ] = {
	"cgameVM", "endFrame", "svFrame",
	// Subsets of endFrame - indented so a reader does not add them to the
	// column above and find the frame over-explained.
	" geSync", " vblank"
};

void Sys_PSP_ZoneBegin( int zone )
{
	if( zone < 0 || zone >= PSP_ZONE_COUNT ) {
		return;
	}

	psp_zoneStart[ zone ] = sceKernelGetSystemTimeLow();
	psp_zoneOpen[ zone ]  = 1;
}

void Sys_PSP_ZoneEnd( int zone )
{
	unsigned int us;

	if( zone < 0 || zone >= PSP_ZONE_COUNT || !psp_zoneOpen[ zone ] ) {
		return;
	}

	us = sceKernelGetSystemTimeLow() - psp_zoneStart[ zone ];
	psp_zoneOpen[ zone ] = 0;

	psp_zoneUs[ zone ] += us;
	if( us > psp_zoneMaxUs[ zone ] ) {
		psp_zoneMaxUs[ zone ] = us;
	}
}

void Sys_PSP_FrameMark( void )
{
	const unsigned int now = sceKernelGetSystemTimeLow();
	unsigned int windowUs, accounted, i, meanUs, medianUs, p95Us;

	if( !psp_frameLastUs ) {
		psp_frameLastUs   = now;
		psp_frameWindowUs = now;
		return;
	}

	{
		const unsigned int frameUs = now - psp_frameLastUs;

		if( frameUs > psp_frameMaxUs ) {
			psp_frameMaxUs = frameUs;
		}
		{
			unsigned int bin = frameUs / PSP_FRAME_HISTOGRAM_BIN_US;

			if( bin >= PSP_FRAME_HISTOGRAM_BINS ) {
				bin = PSP_FRAME_HISTOGRAM_BINS - 1;
				psp_frameHistogramOverflow++;
			}
			psp_frameHistogram[ bin ]++;
		}
	}

	psp_frameLastUs = now;
	psp_frameCount++;

	windowUs = now - psp_frameWindowUs;
	if( windowUs < 5000000 ) {
		return;
	}

	if( !psp_frameCount ) {
		return;
	}

	meanUs   = windowUs / psp_frameCount;
	medianUs = Sys_PSP_FramePercentileUs( 50 );
	/* p95 frame time is the 5%-low threshold; ceiling rank is intentional. */
	p95Us = Sys_PSP_FramePercentileUs( 95 );

	Com_Printf( "PSP perf: build %s, %u frames in %u ms, 1.000 ms histogram%s\n",
		PSP_PERF_BUILD_ID, psp_frameCount, windowUs / 1000,
		psp_frameHistogramOverflow ? " (>=1024 ms frame observed)" : "" );
	Com_Printf( "PSP frame: mean %u.%03u ms, median %u.%03u ms, "
		"p95 %u.%03u ms (5%% low), worst %u.%03u ms, %u.%u fps derived\n",
		meanUs / 1000, meanUs % 1000,
		medianUs / 1000, medianUs % 1000,
		p95Us / 1000, p95Us % 1000,
		psp_frameMaxUs / 1000, psp_frameMaxUs % 1000,
		1000000 / meanUs, ( 10000000 / meanUs ) % 10 );

	accounted = 0;
	for( i = 0; i < PSP_ZONE_COUNT; i++ ) {
		// Only the top-level zones partition the frame; the rest are subsets
		// of one of them (psp_platform.h) and would be counted twice.
		if( i < PSP_ZONE_TOPLEVEL )
			accounted += psp_zoneUs[ i ];

		Com_Printf( "PSP frame:   %-8s %u ms/frame, share %u%%, worst %u ms\n",
			psp_zoneName[ i ],
			psp_zoneUs[ i ] / ( psp_frameCount * 1000 ),
			( psp_zoneUs[ i ] * 100 ) / windowUs,
			psp_zoneMaxUs[ i ] / 1000 );
	}

	// endFrame minus both waits is the CPU building the frame; if geSync dominates, the GE is the
	// wall instead. Derived, so no timestamp lands inside PSP_DrawElements.
	{
		unsigned int waits = psp_zoneUs[ PSP_ZONE_GESYNC ] +
			psp_zoneUs[ PSP_ZONE_VBLANK ];
		unsigned int cpuGfx = psp_zoneUs[ PSP_ZONE_ENDFRAME ] > waits ?
			psp_zoneUs[ PSP_ZONE_ENDFRAME ] - waits : 0;

		Com_Printf( "PSP frame:   %-8s %u ms/frame, share %u%% (endFrame minus the waits)\n",
			"gfxCPU",
			cpuGfx / ( psp_frameCount * 1000 ),
			( cpuGfx * 100 ) / windowUs );
	}

	// Across a map load, zone time can exceed the window; clamp so unsigned subtraction does not
	// print a ~4 billion ms "other". The frame count marks such a window.
	if( accounted > windowUs ) {
		accounted = windowUs;
	}

	Com_Printf( "PSP frame:   %-8s %u ms/frame, share %u%%, vertex arena peak %d KB\n",
		"other",
		( windowUs - accounted ) / ( psp_frameCount * 1000 ),
		( ( windowUs - accounted ) * 100 ) / windowUs,
		PSP_DrawArenaPeakKB() );

#ifdef PSP_RENDER_PROFILE
	Sys_PSP_RenderProfileReport( windowUs, psp_frameCount );
#endif
	Sys_PSP_CountReport( "window", psp_frameCount );
	PSP_StaticWorld_Report();
	Sys_PSP_FileTraceReport();
	PSP_AdpcmCacheReport();

	for( i = 0; i < PSP_ZONE_COUNT; i++ ) {
		psp_zoneUs[ i ]    = 0;
		psp_zoneMaxUs[ i ] = 0;
	}

	psp_frameWindowUs = now;
	psp_frameCount    = 0;
	psp_frameMaxUs    = 0;
	psp_frameHistogramOverflow = 0;
	for( i = 0; i < PSP_FRAME_HISTOGRAM_BINS; i++ ) {
		psp_frameHistogram[ i ] = 0;
	}
}

// For callers after main() resolved it (con_psp.c, the Sys_Default* path functions).
char *Sys_PSP_BasePath( void )
{
	if( !psp_basePathResolved )
		return Sys_PSP_ResolveBasePath( NULL );

	return psp_basePath;
}

// Called right after CON_Init: the log is the only observation channel from the XMB.
void Sys_PSP_PrintBootDiagnostics( void )
{
	// Build stamp first: a log that does not carry the expected timestamp is a
	// stale EBOOT on the stick, and every line below it is then meaningless.
	Sys_Print( va( "PSP: build %s, product %s, compiled " __DATE__ " " __TIME__ "\n",
		PSP_PERF_BUILD_ID, PRODUCT_VERSION ) );
	Sys_Print( psp_bootTrace );
	Sys_Print( va( "PSP: base path '%s'\n", Sys_PSP_BasePath() ) );
	Sys_Print( va( "PSP: max free user mem %u KB\n",
		(unsigned int)( sceKernelMaxFreeMemSize() / 1024 ) ) );

#ifdef PSP_BOOT_TRACE
	// Heap state around the first malloc: libcglue claims the block lazily in the first _sbrk.
	// __psp_heap_blockid is 0 before it and negative if the partition allocation failed.
	{
		extern int    __psp_heap_blockid;
		extern char   _end;
		void          *p;

		int i;

		Sys_Print( va( "PSP: bss  %08x..%08x (%u KB), nonzero words %u, first %08x\n",
			psp_bssStart, psp_bssEnd, ( psp_bssEnd - psp_bssStart ) / 1024,
			psp_bssNonZeroTotal, psp_bssFirstNonZero ) );

		for( i = 0; i < PSP_BSS_BUCKETS; i += 4 )
			Sys_Print( va( "PSP: bss  bucket %2d-%2d: %d %d %d %d\n", i, i + 3,
				psp_bssBucketNonZero[ i ], psp_bssBucketNonZero[ i + 1 ],
				psp_bssBucketNonZero[ i + 2 ], psp_bssBucketNonZero[ i + 3 ] ) );

		Sys_Print( va( "PSP: &__psp_heap_blockid %p = %d\n",
			(void *)&__psp_heap_blockid, __psp_heap_blockid ) );

		Sys_Print( va( "PSP: heap cfg  request %d KB, threshold default 512 KB\n",
			sce_newlib_heap_kb_size ) );
		Sys_Print( va( "PSP: image end &_end %p\n", (void *)&_end ) );
		Sys_Print( va( "PSP: pre-malloc  blockid %d, sbrk(0) %p, free %u KB\n",
			__psp_heap_blockid, sbrk( 0 ),
			(unsigned int)( sceKernelMaxFreeMemSize() / 1024 ) ) );

		p = malloc( 16 );

		Sys_Print( va( "PSP: post-malloc blockid %d, sbrk(0) %p, free %u KB, ptr %p\n",
			__psp_heap_blockid, sbrk( 0 ),
			(unsigned int)( sceKernelMaxFreeMemSize() / 1024 ), p ) );

		if( __psp_heap_blockid > 0 )
			Sys_Print( va( "PSP: block head %p\n",
				sceKernelGetBlockHeadAddr( __psp_heap_blockid ) ) );

		if( p )
			free( p );
	}
#endif
}

// Boot budget and launch profile as "+set" commands (Wii pattern). r_primitives 2 is required:
// auto picks per-vertex qglArrayElement, a no-op here. cl_motd 0 avoids a dead-host DNS stall.
void Sys_PSP_BuildBootCommandLine( char *cmdline, int size )
{
	int  heapMB     = PSP_HEAP_KB / 1024;
	int  outsideKB  = (int)( sceKernelMaxFreeMemSize() / 1024 );
	int  hunkMB;
	int  bufLen;
	char buf[ MAX_STRING_CHARS ];

	// The hunk is calloc'd from the heap, so size it from PSP_HEAP_KB, never MaxFreeMemSize;
	// the reserve covers zone, vertex arena, display list, libc and texture spill. 27 is the mirror's cap.
	hunkMB = heapMB - PSP_HUNK_RESERVE_MB;
	if( hunkMB > 27 )
		hunkMB = 27;
	if( hunkMB < MIN_COMHUNKMEGS )
		hunkMB = MIN_COMHUNKMEGS;

	// vm_* 0 is VMI_NATIVE, the linked-in modules; without one, FS_FindVM falls back to the .qvm.
	bufLen = Com_sprintf( buf, sizeof( buf ),
		"+set com_hunkMegs %d "
		"+set com_zoneMegs %d "
		"+set com_soundMegs %d "
#if defined(PSP_NATIVE_GAME_MODULES) || defined(PSP_STATIC_GAME_MODULES)
		"+set vm_cgame 0 "
		"+set vm_ui 0 "
#endif
#ifdef PSP_STATIC_GAME_MODULES
		"+set vm_game 0 "
#endif
#ifndef PSP_SESSION21_UNPIN_S_KHZ
		"+set s_khz 11 "
#endif
		"+set sv_pure 0 "
		/* Shipping PSP performance profile. Keep cg_drawfps in autoexec.cfg. */
		"+set r_picmip 1 "
		"+set r_subdivisions 80 "
		"+set r_vertexlight 1 "
		/* Network play needs this pacing path, so the launch line must beat archived config. */
		"+set r_pspNetVblank 1 "
		/* Keep the performance profile ahead of archived config values. */
		"+set r_fastsky 1 "
		/* r_dynamiclight is the real renderer cvar; r_dynamic is not. */
		"+set r_dynamiclight 0 "
		"+set r_flares 0 "
		"+set r_drawSun 0 "
		"+set r_primitives 2 "
		"+set cg_shadows 0 "
		"+set cg_gibs 0 "
		"+set cg_brassTime 0 "
		"+set cg_marks 0 "
		"+set cl_motd 0 ",
		hunkMB, PSP_ZONE_MEGS, PSP_SOUND_MEGS );
	if( bufLen >= (int)sizeof( buf ) ) {
		Sys_Error( "PSP: boot command line needs %d bytes, buf is %d - widen buf[] in Sys_PSP_BuildBootCommandLine",
			bufLen + 1, (int)sizeof( buf ) );
	}

	// Q_strcat truncates silently. Not fatal, since the room depends on the argv[0] path,
	// but it must be logged: a dropped tail once lost cl_motd 0.
	{
		const int used = (int)strlen( cmdline );

		if( used + bufLen >= size ) {
			Sys_Print( va( "PSP: WARNING boot command line overflow, %d bytes of launch cvars dropped "
				"(cmdline %d, adding %d, capacity %d)\n",
				( used + bufLen ) - ( size - 1 ), used, bufLen, size ) );
		}
	}

	Q_strcat( cmdline, size, buf );

	// outsideKB is the partition left after the heap; net modules load there, so if it runs
	// short, lower PSP_HEAP_KB rather than the hunk.
	Sys_Print( va( "PSP: heap %d MB, outside-heap free %d KB (PRX budget), "
		"chosen hunk %d MB, zone %d MB, sound %d MB\n", heapMB, outsideKB,
		hunkMB, PSP_ZONE_MEGS, PSP_SOUND_MEGS ) );
}

// Other engine code reads sys_timeBase/curtime directly, as with sys_unix.c.
// sceRtcGetCurrentTick counts microseconds; only the delta since the first call matters.
unsigned long sys_timeBase = 0;
int curtime;

int Sys_Milliseconds( void )
{
	u64 tick;
	u32 resolution = sceRtcGetTickResolution();
	unsigned long sec, usec;

	sceRtcGetCurrentTick( &tick );

	sec  = (unsigned long)( tick / resolution );
	usec = (unsigned long)( tick % resolution );

	if( !sys_timeBase )
	{
		sys_timeBase = sec;
		return (int)( usec / 1000 );
	}

	curtime = (int)( ( sec - sys_timeBase ) * 1000 + usec / 1000 );

	return curtime;
}

// Matches the mirror's minimal init, not Daedalus's: importing Daedalus's wholesale froze a
// PSP-2000. The Slim gate, VRAM and volatile memory are set up where they are first used.
void Sys_PlatformInit( void )
{
	int rc;

	PSP_SetupExitCallback();

	// A refused request would run the session at 222 MHz, which reads like slow code, so the
	// clock is read back from the hardware and logged. A slow boot beats no boot.
	rc = scePowerSetClockFrequency( 333, 333, 166 );
	PSP_TraceAppend( "PSP: clock set rc %08x -> cpu %d MHz, bus %d MHz\n",
		rc, scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency() );

	pspFpuSetEnable( 0 ); // disable FPU exceptions

	pspDebugScreenInit();
}

void Sys_PlatformExit( void )
{
}

// No-ops, as in sys_unix.c: there is no "safe mode" video path.
void Sys_GLimpSafeInit( void )
{
}

void Sys_GLimpInit( void )
{
}

// newlib provides POSIX file calls over sceIo, so these are sys_unix.c's without XDG or dialogs.
const char *Sys_Basename( char *path )
{
	static char base[ MAX_OSPATH ];
	char *slash;

	if( !path || !*path )
		return ".";

	slash = strrchr( path, PATH_SEP );
	if( !slash )
		Q_strncpyz( base, path, sizeof( base ) );
	else
		Q_strncpyz( base, slash + 1, sizeof( base ) );

	return base;
}

const char *Sys_Dirname( char *path )
{
	static char dir[ MAX_OSPATH ];
	char *slash;

	if( !path || !*path )
		return ".";

	Q_strncpyz( dir, path, sizeof( dir ) );
	slash = strrchr( dir, PATH_SEP );

	if( !slash )
		Q_strncpyz( dir, ".", sizeof( dir ) );
	else if( slash == dir )
		dir[ 1 ] = '\0';
	else
		*slash = '\0';

	return dir;
}

FILE *Sys_FOpen( const char *ospath, const char *mode )
{
	struct stat buf;
	int statResult;
#ifdef PSP_STUTTER_TRACE
	unsigned int traceStart;
#endif

	#ifdef PSP_STUTTER_TRACE
	traceStart = Sys_PSP_RenderProfileNow();
	#endif
	statResult = stat( ospath, &buf );
	#ifdef PSP_STUTTER_TRACE
	Sys_PSP_StutterTraceRecord( PSP_STUTTER_PHASE_LOOSE_STAT,
		Sys_PSP_RenderProfileNow() - traceStart, -1, -1, 0, 0, 0,
		statResult, 0, NULL );
	#endif

	if( !statResult && S_ISDIR( buf.st_mode ) )
		return NULL;

	#ifdef PSP_STUTTER_TRACE
	traceStart = Sys_PSP_RenderProfileNow();
	#endif
	{
		FILE *file = fopen( ospath, mode );
#ifdef PSP_STUTTER_TRACE
		Sys_PSP_StutterTraceRecord( PSP_STUTTER_PHASE_LOOSE_FOPEN,
			Sys_PSP_RenderProfileNow() - traceStart, -1, -1, 0, 0, 0,
			file ? 0 : -1, 0, NULL );
#endif
		return file;
	}
}

qboolean Sys_Mkdir( const char *path )
{
	int result = mkdir( path, 0777 );

	if( result != 0 )
		return errno == EEXIST;

	return qtrue;
}

FILE *Sys_Mkfifo( const char *ospath )
{
	// No named-pipe support on PSP; the journal/fifo feature this backs is
	// optional debug tooling, not part of the boot path.
	return NULL;
}

// Not getcwd(): the CWD depends on the launcher and every caller wants the install directory.
char *Sys_Cwd( void )
{
	return Sys_PSP_BasePath();
}

char *Sys_BinaryPathRelative( const char *relative )
{
	static char resolved[ MAX_OSPATH ];

	Com_sprintf( resolved, sizeof( resolved ), "%s%c%s", Sys_BinaryPath(), PATH_SEP, relative );

	return resolved;
}

char *Sys_SteamPath( void )
{
	return "";
}

char *Sys_GogPath( void )
{
	return "";
}

char *Sys_MicrosoftStorePath( void )
{
	return "";
}

// One directory for everything: a PSP has no home directory, and three would only invent
// paths the user then has to find.
char *Sys_DefaultHomeConfigPath( void )
{
	return Sys_PSP_BasePath();
}

char *Sys_DefaultHomeDataPath( void )
{
	return Sys_PSP_BasePath();
}

char *Sys_DefaultHomeStatePath( void )
{
	return Sys_PSP_BasePath();
}

#define MAX_FOUND_FILES 0x1000

static void Sys_ListFilteredFiles( const char *basedir, char *subdirs, char *filter, char **list, int *numfiles )
{
	char          search[ MAX_OSPATH ], newsubdirs[ MAX_OSPATH ];
	char          filename[ MAX_OSPATH ];
	DIR           *fdir;
	struct dirent *d;
	struct stat   st;

	if( *numfiles >= MAX_FOUND_FILES - 1 )
		return;

	if( basedir[ 0 ] == '\0' )
		return;

	if( strlen( subdirs ) )
		Com_sprintf( search, sizeof( search ), "%s/%s", basedir, subdirs );
	else
		Com_sprintf( search, sizeof( search ), "%s", basedir );

	if( ( fdir = opendir( search ) ) == NULL )
		return;

	while( ( d = readdir( fdir ) ) != NULL )
	{
		Com_sprintf( filename, sizeof( filename ), "%s/%s", search, d->d_name );
		if( stat( filename, &st ) == -1 )
			continue;

		if( st.st_mode & S_IFDIR )
		{
			if( Q_stricmp( d->d_name, "." ) && Q_stricmp( d->d_name, ".." ) )
			{
				if( strlen( subdirs ) )
					Com_sprintf( newsubdirs, sizeof( newsubdirs ), "%s/%s", subdirs, d->d_name );
				else
					Com_sprintf( newsubdirs, sizeof( newsubdirs ), "%s", d->d_name );
				Sys_ListFilteredFiles( basedir, newsubdirs, filter, list, numfiles );
			}
		}
		if( *numfiles >= MAX_FOUND_FILES - 1 )
			break;
		Com_sprintf( filename, sizeof( filename ), "%s/%s", subdirs, d->d_name );
		if( !Com_FilterPath( filter, filename, qfalse ) )
			continue;
		list[ *numfiles ] = CopyString( filename );
		( *numfiles )++;
	}

	closedir( fdir );
}

char **Sys_ListFiles( const char *directory, const char *extension, char *filter, int *numfiles, qboolean wantsubs )
{
	struct dirent *d;
	DIR           *fdir;
	qboolean      dironly = wantsubs;
	char          search[ MAX_OSPATH ];
	int           nfiles;
	char          **listCopy;
	char          *list[ MAX_FOUND_FILES ];
	int           i;
	struct stat   st;
	int           extLen;

	if( filter )
	{
		nfiles = 0;
		Sys_ListFilteredFiles( directory, "", filter, list, &nfiles );

		list[ nfiles ] = NULL;
		*numfiles = nfiles;

		if( !nfiles )
			return NULL;

		listCopy = Z_Malloc( ( nfiles + 1 ) * sizeof( *listCopy ) );
		for( i = 0; i < nfiles; i++ )
			listCopy[ i ] = list[ i ];
		listCopy[ i ] = NULL;

		return listCopy;
	}

	if( directory[ 0 ] == '\0' )
	{
		*numfiles = 0;
		return NULL;
	}

	if( !extension )
		extension = "";

	if( extension[ 0 ] == '/' && extension[ 1 ] == 0 )
	{
		extension = "";
		dironly = qtrue;
	}

	extLen = strlen( extension );

	nfiles = 0;

	if( ( fdir = opendir( directory ) ) == NULL )
	{
		*numfiles = 0;
		return NULL;
	}

	while( ( d = readdir( fdir ) ) != NULL )
	{
		Com_sprintf( search, sizeof( search ), "%s/%s", directory, d->d_name );
		if( stat( search, &st ) == -1 )
			continue;
		if( ( dironly && !( st.st_mode & S_IFDIR ) ) ||
			( !dironly && ( st.st_mode & S_IFDIR ) ) )
			continue;

		if( *extension )
		{
			if( strlen( d->d_name ) < extLen ||
				Q_stricmp( d->d_name + strlen( d->d_name ) - extLen, extension ) )
				continue;
		}

		if( nfiles == MAX_FOUND_FILES - 1 )
			break;
		list[ nfiles ] = CopyString( d->d_name );
		nfiles++;
	}

	list[ nfiles ] = NULL;

	closedir( fdir );

	*numfiles = nfiles;

	if( !nfiles )
		return NULL;

	listCopy = Z_Malloc( ( nfiles + 1 ) * sizeof( *listCopy ) );
	for( i = 0; i < nfiles; i++ )
		listCopy[ i ] = list[ i ];
	listCopy[ i ] = NULL;

	return listCopy;
}

void Sys_FreeFileList( char **list )
{
	int i;

	if( !list )
		return;

	for( i = 0; list[ i ]; i++ )
		Z_Free( list[ i ] );

	Z_Free( list );
}

void Sys_Sleep( int msec )
{
	if( msec <= 0 )
		return;

	sceKernelDelayThread( (SceUInt)msec * 1000 );
}

qboolean Sys_RandomBytes( byte *string, int len )
{
	int i;
	u64 tick;

	// No /dev/urandom on PSP. Seed off the RTC tick once; good enough for
	// challenge tokens, not a cryptographic guarantee.
	static qboolean seeded = qfalse;
	if( !seeded )
	{
		sceRtcGetCurrentTick( &tick );
		srand( (unsigned int)tick );
		seeded = qtrue;
	}

	for( i = 0; i < len; i++ )
		string[ i ] = (byte)( rand() & 0xff );

	return qtrue;
}

// XMB nickname made safe for userinfo, or NULL when unset or unreadable.
static const char *Sys_PSP_Nickname( void )
{
	static char nick[MAX_NAME_LENGTH];
	static qboolean resolved = qfalse;

	if( !resolved ) {
		char raw[128];
		int i, n = 0;

		resolved = qtrue;
		if( sceUtilityGetSystemParamString( PSP_SYSTEMPARAM_ID_STRING_NICKNAME, raw, sizeof( raw ) ) == 0 ) {
			raw[ sizeof( raw ) - 1 ] = '\0';
			for( i = 0; raw[ i ] && n < (int)sizeof( nick ) - 1; i++ ) {
				unsigned char c = (unsigned char)raw[ i ];

				// Drop UTF-8 bytes, controls and the userinfo/command delimiters.
				if( c < 0x20 || c > 0x7e || c == '\\' || c == '"' || c == ';' )
					continue;
				if( c == ' ' && n == 0 )
					continue;
				nick[ n++ ] = (char)c;
			}
			while( n > 0 && nick[ n - 1 ] == ' ' )
				n--;
			nick[ n ] = '\0';
		}
	}

	return nick[ 0 ] ? nick : NULL;
}

const char *Sys_PSP_DefaultPlayerName( void )
{
	const char *nick = Sys_PSP_Nickname();

	return nick ? nick : "UnnamedPlayer";
}

char *Sys_GetCurrentUser( void )
{
	const char *nick = Sys_PSP_Nickname();

	return nick ? (char *)nick : "player";
}

qboolean Sys_LowPhysicalMemory( void )
{
	return ( sceKernelMaxFreeMemSize() < ( 8 * 1024 * 1024 ) ) ? qtrue : qfalse;
}

void Sys_ErrorDialog( const char *error )
{
	// No dialog subsystem on PSP - print to the debug screen/log and let
	// the caller (Sys_Error) exit.
	Sys_Print( va( "%s\n", error ) );
}

dialogResult_t Sys_Dialog( dialogType_t type, const char *message, const char *title )
{
	Sys_Print( va( "%s: %s\n", title, message ) );
	return DR_OK;
}

void Sys_SetEnv( const char *name, const char *value )
{
	// No environment on PSP user-mode.
}

int Sys_PID( void )
{
	return 1;
}

qboolean Sys_PIDIsRunning( int pid )
{
	return qfalse;
}

qboolean Sys_DllExtension( const char *name )
{
	return COM_CompareExtension( name, DLL_EXT );
}

qboolean Sys_OpenFolderInPlatformFileManager( const char *path )
{
	return qfalse;
}

qboolean Sys_SetMaxFileLimit( void )
{
	// No rlimit on PSP.
	return qtrue;
}
