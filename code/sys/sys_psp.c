/*
===========================================================================
PSP port - code/sys/sys_psp.c

The sys_unix.c counterpart for the PSP. Implements the full sys_local.h /
qcommon.h Sys_* platform contract, plus the PSP boot boilerplate
(PSP_MODULE_INFO, exit-callback thread, clock, Slim detection, volatile
memory) from DaedalusX64 Source/SysPSP/main.cpp:79-130 (Q3PORT.md 1.3) and
the mirror's callback thread (Quake3PSP-mirror/unix/unix_main.cpp:83-112,
Q3PORT.md 1.2).

Deliberately NOT ported from sys_unix.c: the entire XDG home-directory
migration block (sys_unix.c:123-441), fork/exec dialogs, signal-based
crash dialogs, mmap. None of that exists in a PSP CFW user-mode context.
Session 3 replaces the placeholder home/cwd paths with real ms0:/ef0:
probing derived from argv[0] (Q3PORT.md Session 3).
===========================================================================
*/

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
/*
 MUST be an explicit POSITIVE size. Negative values are broken here.

 A negative PSP_HEAP_SIZE_KB means "claim all free memory MINUS this much",
 which is a runtime computation. On this toolchain + ARK-4 that computation
 yields a corrupt heap: newlib faults inside malloc_extend_top
 (_mallocr.c:2226) the first time malloc extends the top, storing through a
 wild pointer (observed t1 = 0x03256508; valid PSP user RAM starts at
 0x08800000). The fault happens during startup, before any output, which is
 why every earlier build showed only a black screen.

 Measured on a PSP-2000 / ARK-4 via psplink (diag/psp_heap):
   no directive -> crash      -4096 -> crash      -8192 -> crash
   +20480       -> OK, MaxFree 50,324 KB
   +49152       -> OK, MaxFree 55,641 KB

 Note -8192 is exactly what Quake3PSP-mirror/unix/unix_main.cpp:64 uses. It
 works in the mirror's own build (build.mak, PSP_FW_VERSION 500, period GCC)
 and crashes here. This is the one place a mirror value does NOT transfer.

 Sizing: the Session 8 run measured the whole user partition at 36 MB heap +
 2525 KB free + ~12.4 MB module image == ~51 MB. The three budget cvars all
 come out of the heap - com_hunkMegs 24 (calloc, Session 3) + com_zoneMegs 5
	 + com_soundMegs 2 (~6.2 MB, one unit in volatile memory) == ~32 MB - and so does every texture, which is
 what the remainder is for.

 com_soundMegs is NOT megabytes. SND_setup (client/snd_mem.c:82-85) allocates
 cv->integer * 1536 sndBuffers of 2060 bytes each, i.e. ~3.0 MB per unit -
 upstream's default of 8 would be 24 MB on its own. The current launch value is
 2; one 3.09 MB segment is reserved in the volatile pool and the other stays
 in the normal heap. The pool evicts least-recently-used sounds when it fills
 (SND_malloc ->
 S_FreeOldestSound), so it degrades into reloading, not into failing. The
	 PSP sound eviction protects a small set of frequent
	 player sounds before falling back to the ordinary least-recently-used path.

 Session 4 retunes this against the measured PRX footprint, and Session 10
 against measured peak hunk usage.
*/
#ifndef PSP_HEAP_KB
#define PSP_HEAP_KB 39936
#endif
PSP_HEAP_SIZE_KB( PSP_HEAP_KB );

/*
 Everything in the heap that is NOT the hunk, in MB: zone, sound pool, small
 zone, newlib/stdio, and textures - textures being the open-ended remainder
 (Sys_PSP_BuildBootCommandLine explains the split). A knob because Session 11a
 left both pools starving and the next round decides the boundary from the
 hunk/heap figures Sys_PSP_HeapReport now prints, not from a guess.
*/
#ifndef PSP_HUNK_RESERVE_MB
#define PSP_HUNK_RESERVE_MB 15
#endif

/*
 The sound and zone budgets are compile-time knobs so a candidate build can
 change only those pools while keeping PSP_HEAP_KB and PSP_HUNK_RESERVE_MB
 fixed. Test 2 uses sound = 2 and zone = 3.
*/
#ifndef PSP_SOUND_MEGS
#define PSP_SOUND_MEGS 2
#endif
#ifndef PSP_ZONE_MEGS
#define PSP_ZONE_MEGS 5
#endif

/*
 PSPSDK's default main-thread stack is 256 KB. xash3d-fwgs, a PSP port proven
 on hardware, raises it to 512 KB (engine/platform/psp/sys_psp.c:33) and we
 have the same reason to: Sys_ListFiles puts char *list[MAX_FOUND_FILES] ==
 16 KB on the stack and Sys_ListFilteredFiles recurses per subdirectory.
*/
PSP_MAIN_THREAD_STACK_SIZE_KB( 512 );

// Last-resort probe candidates, in order. A PSP Go has no ms0: at all, so ef0:
// is tried first (on a Go both may exist; ef0: is where the EBOOT lives).
#define PSP_EF0_BASE_PATH "ef0:/PSP/GAME/ioquake3"
#define PSP_MS0_BASE_PATH "ms0:/PSP/GAME/ioquake3"

/*
===========================================================================
Exit callback thread (HOME button)

Without this the firmware's exit dialog hangs the app - Q3PORT.md 5.
===========================================================================
*/

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

/*
===========================================================================
Base path resolution

Q3PORT.md 5, "Works via psplink, file not found from XMB": the CWD depends
on the launcher, so every path must be absolute and derived at boot.

Candidate order:
  1. dirname(argv[0]), if it carries a device prefix and is a directory.
     The XMB passes the full EBOOT path; psplink passes host0:/...
  2. getcwd(). This is what Quake3PSP-mirror uses and it is the only
     mechanism proven on real hardware for this engine
     (unix/unix_shared.c:364, unix/unix_main.cpp:524). PSPSDK sets the CWD
     to the directory the EBOOT was launched from.
  3. The hardcoded ef0:/ then ms0:/ install paths.

Resolution runs from main() before CON_Init, so nothing can be printed at
the time. The attempt trace is recorded into a static buffer and dumped by
Sys_PSP_PrintBootDiagnostics() once the log exists.
===========================================================================
*/

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

/*
==================
PSP_IsDirectory

sceIoGetstat rather than stat(): this runs before the engine is up, and it
reports the PSP error code directly, which is what the trace wants.
==================
*/
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

/*
==================
PSP_HasDevicePrefix

"ms0:/..." / "ef0:/..." / "host0:/..." / "disc0:/...". A path without one is
launcher-relative and useless to us.
==================
*/
static qboolean PSP_HasDevicePrefix( const char *path )
{
	const char *colon = strchr( path, ':' );

	// The prefix must come first and be non-empty: "ms0:" yes, "/foo:bar" no.
	return ( colon && colon != path && strchr( path, '/' ) != NULL &&
		colon < strchr( path, '/' ) ) ? qtrue : qfalse;
}

/*
==================
Sys_PSP_ResolveBasePath
==================
*/
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

/*
==================
Sys_PSP_HeapReport

DIAGNOSTIC - Session 7 map-load failure. See the block comment in
code/psp/psp_platform.h for why this exists and what it has to decide.

mallinfo() reports the newlib heap, i.e. the PSP_HEAP_SIZE_KB block
libcglue claimed on the first _sbrk - which is where the hunk, the zone and
every psp_tex.c texture live. sceKernelMaxFreeMemSize() reports what is
left OUTSIDE it, which is a different pool and is the Session 4 PRX budget.
Both are printed because confusing the two is exactly the mistake
Sys_PSP_BuildBootCommandLine already documents.

The fopen probe is the decisive one: if a plain fopen of the same path
succeeds right after unzOpen failed, the problem is heap and not the
Memory Stick or a file-descriptor limit.
===========================================================================
*/
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

	/*
	 The hunk is calloc'd out of the same heap, so the two numbers have to be
	 read together: heap pressure is not a hunk shortage and vice versa. It is
	 reported here because Session 11a's re-budget cannot be decided without
	 it - the ~8 MB the cgame and ui QVMs used to hold came back as hunk
	 headroom, and whether that headroom is slack or is being spent by the map
	 is exactly what Sys_PSP_BuildBootCommandLine's "heapMB - 15" is guessing
	 at. Hunk_MemoryRemaining is the low+high gap, i.e. what is still
	 allocatable, and reads 0 before Com_InitHunkMemory.
	*/
	Sys_Print( va( "PSP hunk [%s]: %d KB free of com_hunkMegs %d\n",
		where ? where : "?",
		Hunk_MemoryRemaining() / 1024,
		(int)Cvar_VariableValue( "com_hunkMegs" ) ) );

	/*
	 File handles are reported alongside the heap because the first Session 7
	 map load failed on handles while the heap still had room, and the two
	 numbers were indistinguishable in the log.
	*/
	PSP_VF_ReportInto( vf, sizeof( vf ) );
	Sys_Print( va( "PSP %s\n", vf ) );
}

/*
===========================================================================
Sys_PSP_ScanBss

Measures whether the loader actually zeroed .bss, and if it stopped, where.

Motive: __psp_heap_blockid (libcglue, .sbss) was observed holding 0x08960000
before the first malloc in the process, when it must be 0. libcglue guards
its entire heap setup with "if (heap_bottom == NULL)" over a .bss static
(pspsdk-src/src/libcglue/glue.c:691,697); non-zero garbage there means the
heap is never created and every malloc returns NULL - exactly what the
hardware reports.

Must run as the FIRST statement in main(), before anything writes to .bss.
Results are stashed and printed later, once the log exists.
==================
*/

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

/*
===========================================================================
Native game modules - Sys_LoadDll / Sys_LoadGameDll / Sys_UnloadDll

Session 10. Replaces the interpreted cgame and ui QVMs, which the frame
breakdown measured at ~40% of a 124 ms frame, with PRX modules of native
Allegrex code. The QVM interpreter cannot be offloaded or compiled away on
this platform - there is no MIPS bytecode compiler in ioquake3 (vm_x86,
vm_powerpc, vm_sparc, vm_armv7l, and no vm_mips) - so going native is the
only route to that 40%.

How this differs from every other port
--------------------------------------
Upstream resolves entry points by name with dlsym/GetProcAddress/
SDL_LoadFunction. The PSP has no such call: a PRX exports by NID and the
LOADER patches the importer's stub table at module start. So the addresses
do not come back from this function at all - they are the link-time symbols
in psp/psp_modules.S, and all this does is load the module, start it, and
hand back the pair that belongs to the name it was asked for.

That is Quake3PSP-mirror's arrangement (unix/unix_main.cpp:504-620 plus
unix/moduleAPI.S), and it is the only Quake 3 known to run game modules
natively on this hardware.

Consequences worth stating, because they are not obvious:

  - The returned "handle" is an index+1 into pspModuleUid, NOT a pointer.
    vm.c only ever tests it for non-NULL and passes it back to
    Sys_UnloadDll, so an opaque small integer is fine - but it must never
    be dereferenced, and 0 must stay reserved for failure.
  - Adding a module means adding BOTH an exports.exp entry in the module
    and a STUB_START group in psp_modules.S, with matching library names.
    A mismatch loads and starts cleanly, then calls into nothing.
  - PSP_MEMORY_PARTITION_USER: these come out of the partition, NOT the
    newlib heap. That is the whole point of the memory re-budget in
    Sys_PSP_BuildBootCommandLine - the ~8 MB of hunk the two QVMs used to
    occupy is what pays for the partition space these need.

Session 11 - the STATIC route, which is the one that is built
--------------------------------------------------------------
PSP_NATIVE_GAME_MODULES above is off. Loading an unsigned PRX game module at
runtime needs the host EBOOT to be a PRX, and a PRX EBOOT with an 8.6 MB .bss
is refused at load time on this hardware - the two requirements are in direct
conflict (cmake/platforms/psp.cmake). PSP_STATIC_GAME_MODULES links cgame and
ui INTO the EBOOT instead, so there is no loader and no conflict.

Everything above about entry points still holds; only the loading disappears.
The same four symbols are still what gets called, they are just real
definitions now instead of import stubs the loader has to patch, which means
they are callable immediately and can never fail to resolve at runtime.

cmake/basegame.cmake explains how the two modules keep their own symbol
scopes in one link (partial link, then localize everything but the entry
pair) - that, not the calling, is the work in this route.
===========================================================================
*/

#if defined(PSP_NATIVE_GAME_MODULES) || defined(PSP_STATIC_GAME_MODULES)
/*
 With PSP_NATIVE_GAME_MODULES: import stubs from psp/psp_modules.S.
 With PSP_STATIC_GAME_MODULES: the renamed entry points of the two blobs
 cmake/basegame.cmake links in (vmMain -> vmMainCG, dllEntry -> dllEntryCG).
 Declared, never defined here either way.

 vmMain* must be declared with vmMainProc's exact 13-int signature
 (qcommon/qcommon.h:351), not as varargs: a stub has no C type of its own,
 so the declaration is purely how this file agrees with the caller, and
 varargs versus fixed args is a real ABI difference on MIPS - the first four
 arguments go in registers either way, but the compiler is entitled to lay
 out the rest differently.
*/
extern void	dllEntryCG( intptr_t ( QDECL *syscallptr )( intptr_t, ... ) );
extern intptr_t	QDECL vmMainCG( int callNum, int arg0, int arg1, int arg2,
			int arg3, int arg4, int arg5, int arg6, int arg7, int arg8,
			int arg9, int arg10, int arg11 );

extern void	dllEntryUI( intptr_t ( QDECL *syscallptr )( intptr_t, ... ) );
extern intptr_t	QDECL vmMainUI( int callNum, int arg0, int arg1, int arg2,
			int arg3, int arg4, int arg5, int arg6, int arg7, int arg8,
			int arg9, int arg10, int arg11 );

#ifdef PSP_STATIC_GAME_MODULES
// qagame is static-only: the PRX build never shipped stubs for it (there is
// nothing to load a module against in psp_modules.S).
extern void	dllEntryQAG( intptr_t ( QDECL *syscallptr )( intptr_t, ... ) );
extern intptr_t	QDECL vmMainQAG( int callNum, int arg0, int arg1, int arg2,
			int arg3, int arg4, int arg5, int arg6, int arg7, int arg8,
			int arg9, int arg10, int arg11 );
#endif

#ifdef PSP_NATIVE_GAME_MODULES
#define PSP_MAX_GAME_MODULES	3

static SceUID	pspModuleUid[ PSP_MAX_GAME_MODULES ];

/*
==================
PSP_ModuleEntryPoints

Maps a module name to the stub pair linked for it. Returns qfalse for a name
this build has no stubs for - which is how qagame currently fails, rather
than by loading a module nothing can call.
==================
*/
/*
==================
PSP_ModuleEntryPoints

Writes into CALLER LOCALS, never into vm_t. Sys_LoadGameDll must not touch
*entryPoint until the module has actually started, because vm.c keeps going
after a failed native load:

    vm->dllHandle = Sys_LoadGameDll(filename, &vm->entryPoint, ...);
    if(vm->dllHandle) { ...; return vm; }
    Com_Printf("Failed loading dll, trying next\n");     <- falls through
    ... VM_LoadQVM ...

and VM_Call then dispatches on that same field (qcommon/vm.c:826):

    if ( vm->entryPoint )  r = vm->entryPoint( ... );
    else                   r = VM_CallInterpreted( ... );

An early write leaves vm->entryPoint pointing at an UNPATCHED import stub,
so the interpreter is bypassed and every VM_Call returns the stub's error
code instead. That is the Session 10 hardware failure:

    ERROR: User Interface is version -2147352262, expected 6

-2147352262 is 0x8002013A - an unresolved-import stub's return value, not a
version number. The QVM fallback had loaded correctly and was never called.
==================
*/
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

/*
==================
PSP_ModuleBaseName

FS_FindVM hands back a full path - "ms0:/PSP/GAME/ioquake3/baseq3/cgame.prx"
- but the stub pair is chosen by module name. Recover it by stripping the
directory and both the extension and the optional ARCH_STRING suffix
FS_FindVM's dllNameFormats may have added ("cgamemips.prx").
==================
*/
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

	/*
	 kuKernelLoadModule, NOT sceKernelLoadModule.

	 The plain user-mode loader refuses an unsigned homebrew PRX:

	   Sys_LoadGameDll(.../ui.prx): sceKernelLoadModule failed (0x80020148)

	 0x80020148 is "unsupported PRX type" - the module is unencrypted, and
	 the user-mode path only accepts signed modules. kubridge's variant runs
	 the load with kernel privileges and is exactly what CFW provides it
	 for. Quake3PSP-mirror reaches the same place from the other direction
	 (unix_main.cpp:537-546): it tries pspSdkLoadStartModule first and falls
	 back to kuKernelLoadModule when that fails.

	 libpspkubridge is already linked and already proven on this hardware -
	 kuKernelGetModel is what prints "PSP model 1" in psp_glimp.c - so this
	 costs no new library and cannot resplit the import stubs (psp.cmake's
	 "stubs out of order" note).

	 sceKernelLoadModule stays as the fallback rather than the primary: if a
	 future firmware or a signed build makes it viable, it is the cheaper
	 path, and trying it second costs one failed call on a path that has
	 already failed once.
	*/
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

	/*
	 The stubs are patched by the loader during sceKernelStartModule, so
	 this is the first moment either pointer is safe to call - and dllEntry
	 must be called before vmMain, because it is what hands the module its
	 syscall trampoline.

	 Publishing *entryPoint here, after the start succeeded, is what keeps
	 the QVM fallback usable on every failure path above.
	*/
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
/*
===========================================================================
Static module state - the part a static link does NOT give you for free.

vm.c destroys and recreates a VM on every map change, vid_restart and
fs_restart. A QVM gets a freshly loaded image each time, and a desktop DLL
gets dlclose + dlopen, so in both cases the module's globals go back to
their initial values. Linked into the EBOOT they simply persist.

That is not academic. Session 11's hardware run flooded the log with

    R_GetShaderByHandle: out of range hShader '283'

once per drawn frame after a demo -> q3dm1 transition: q3_ui caches shader
handles in per-menu statics, Menu_Cache (q3_ui/ui_qmenu.c:1745) refreshes
only the global uis.* set, and RE_Shutdown had meanwhile reset tr.numShaders.
Handles registered against the previous renderer instance were still being
drawn. cgame is not affected - CG_Init memsets cgs, cg, cg_entities,
cg_weapons and cg_items (cgame/cg_main.c:1849-1853) - but "this module
happens to clear the fields that matter" is not a property to rely on.

So reproduce dlopen semantics instead of patching q3_ui: snapshot each
module's .data the first time it is used (nothing in it has run yet, so the
image is pristine), then on every later load restore that snapshot and zero
.bss. Cheap: cgame is 6.0 KB of .data and 1.45 MB of .bss, ui 7.8 KB and
570 KB, against a map load measured in tens of seconds.

The section symbols come from cmake/basegame.cmake, which renames each
blob's .data/.bss to <name>_data/<name>_bss precisely so that ld emits
__start_/__stop_ for them - it only does that for orphan sections whose
names are valid C identifiers, which ".data" is not.
===========================================================================
*/
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

/*
==================
PSP_ModuleResetState

First call per module: keep a copy of .data and leave everything alone -
the module has never run, so .data is the linked image and .bss is the
loader's zeroes.

Later calls: put both back the way the first call found them.

If the snapshot cannot be allocated the module still loads. It then behaves
exactly as it did before this function existed, which is degraded but not
broken, and the reason is printed rather than guessed at later.
==================
*/
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

/*
==================
Sys_LoadGameDll - static route

Everything the PRX version does around the two calls is gone: the module is
part of this binary, so there is nothing to find, load, start or relocate.
What remains is the contract vm.c expects - reset the module's state, hand
back the entry point, run dllEntry to give the module its syscall
trampoline, return a non-NULL handle.

"name" is whatever vm.c passed. VM_Create's PSP branch passes the bare module
name ("cgame"); PSP_ModuleBaseName also accepts a full path, so a future
caller coming through FS_FindVM would still resolve.

The failure path matters more than the success path: returning NULL WITHOUT
having written *entryPoint is what keeps the .qvm in the pk3 usable. See the
note on PSP_ModuleEntryPoints - an early write bypasses the interpreter and
turns every VM_Call into a garbage return value.
==================
*/
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

/*
==================
Sys_UnloadDll - static route

Releases the slot and nothing else; the code stays mapped because it is this
binary. The state does not have to be cleared here - PSP_ModuleResetState
does it on the way back in, where the pristine copy is guaranteed to exist
and a partially torn-down module cannot be left running on zeroed globals.
==================
*/
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

/*
===========================================================================
Sys_PSP_Zone* / Sys_PSP_FrameMark - step 1b frame breakdown.

See the block comment on the declarations in psp/psp_platform.h for why this
exists and what it decides. Timing source is sceKernelGetSystemTimeLow (us);
everything here runs on the main thread, so no synchronisation is needed.

A zone that is entered but never left (an early return between Begin and End)
simply contributes nothing - psp_zoneOpen guards the pairing rather than
letting a stale start timestamp poison the accumulator with a huge delta.
===========================================================================
*/
static unsigned int	psp_zoneStart[ PSP_ZONE_COUNT ];
static int			psp_zoneOpen[ PSP_ZONE_COUNT ];
static unsigned int	psp_zoneUs[ PSP_ZONE_COUNT ];
static unsigned int	psp_zoneMaxUs[ PSP_ZONE_COUNT ];

static unsigned int	psp_frameWindowUs = 0;
static unsigned int	psp_frameCount    = 0;
static unsigned int	psp_frameMaxUs    = 0;
static unsigned int	psp_frameLastUs   = 0;

/*
 A five-second gameplay window contains at most 300 frames at the LCD's
 59.94 Hz refresh. A 1 ms histogram records the distribution without a
 sort or allocation at report time, so the profiler cannot leak a sorting
 spike into the next measured frame. It is static 2 KB storage and covers
 frame-time tails through 1.024 seconds instead of clipping p95 at 256 ms.
*/
#define PSP_FRAME_HISTOGRAM_BIN_US 1000
#define PSP_FRAME_HISTOGRAM_BINS   1024
static unsigned short psp_frameHistogram[ PSP_FRAME_HISTOGRAM_BINS ];
static unsigned int   psp_frameHistogramOverflow = 0;

unsigned int Sys_PSP_RenderProfileNow( void )
{
	return sceKernelGetSystemTimeLow();
}

#ifdef PSP_RENDER_PROFILE
/*
 The first targeted VFPU investigation needs attribution before code changes.
 sceKernelGetSystemTimeLow is deliberately never called in every draw of every
 frame: profile only one renderer frame in sixteen.  This keeps the benchmark
 build usable while still collecting enough representative draws in a five
 second window.  Individual scopes can nest (MD3 lerp calls normalization), so
 the report labels their shares as independent rather than pretending they
 partition the frame.
*/
#define PSP_RPROF_SAMPLE_INTERVAL 16

typedef struct {
	unsigned int start;
	unsigned int total;
	unsigned int calls;
} pspRenderProfileScope_t;

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

/*
 Goal 22 file-read stall attribution.  The counters cover every operation;
 only events at or above 1 ms enter the bounded slow-event list.  Categories
 are deliberately independent, so a restore-open and its parent restore (or
 a codec stage and its lazy-load parent) are reported as overlapping time.
*/
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

/*
===========================================================================
Goal 23 - bounded, post-workload file-lookup trace.

The existing PSP_FILE_TRACE counters remain the cheap live summary.  This
second sink records only complete operations at or above the slow threshold,
while retaining per-phase/per-frame counters for every operation.  Nothing in
this block writes to storage until Sys_PSP_StutterTraceDump is called from
CON_Shutdown, after the measured workload has ended.
===========================================================================
*/
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

static void Sys_PSP_RenderProfileReport( unsigned int windowUs, unsigned int frameCount )
{
	unsigned int i;
	unsigned int meanFrameUs;

	if( !psp_rprofSampledFrames || !frameCount ) {
		memset( psp_rprof, 0, sizeof( psp_rprof ) );
		memset( psp_cgameSyscall, 0, sizeof( psp_cgameSyscall ) );
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

	/* soundPaint is a strict subset of sound, sampled under the same frame
	   decision.  Reporting the difference directly makes the main-CPU A/B
	   useful without putting begin/end timestamps around every non-paint
	   fragment of S_Update. */
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

	memset( psp_rprof, 0, sizeof( psp_rprof ) );
	memset( psp_cgameSyscall, 0, sizeof( psp_cgameSyscall ) );
	psp_rprofSoundAssetCount = 0;
	psp_rprofSampledFrames = 0;
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

	/*
	 The number this instrumentation exists for: endFrame minus the two waits
	 is the CPU building and submitting the frame, and it is what hardware T&L
	 or a VFPU clipper would attack. If geSync dominates instead, the GE is the
	 wall and no amount of CPU work helps.

	 Derived rather than measured so that no timestamp lands inside
	 PSP_DrawElements, which runs hundreds of times per frame.
	*/
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

	// A window spanning a map load accumulates zone time (SCR_UpdateScreen and
	// CL_CGameRendering both run from inside CL_InitCGame) while almost no
	// frame closes, so accounted can exceed the window. Unsigned subtraction
	// would turn that into a ~4 billion ms "other"; report 0 and let the
	// frame count identify the window as the contaminated one.
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
	PSP_StaticWorld_Report();
	Sys_PSP_FileTraceReport();

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

/*
==================
Sys_PSP_BasePath

For callers that run after main() resolved it (con_psp.c, the Sys_Default*
path functions below).
==================
*/
char *Sys_PSP_BasePath( void )
{
	if( !psp_basePathResolved )
		return Sys_PSP_ResolveBasePath( NULL );

	return psp_basePath;
}

/*
==================
Sys_PSP_PrintBootDiagnostics

Called from main() immediately after CON_Init. The XMB/log route is the only
observation channel this session (Q3PORT.md Session 3), so the path decision
has to be readable after the fact.
==================
*/
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
	/*
	 Full heap picture, taken before and after the first malloc in the process.
	 libcglue claims the PSP_HEAP_SIZE_KB block lazily, inside the first _sbrk
	 (pspsdk-src/src/libcglue/glue.c:688-720), so this is the only moment the
	 whole mechanism is observable in one place.

	 sce_newlib_heap_kb_size is the "int sce_newlib_heap_kb_size" that
	 PSP_HEAP_SIZE_KB defines at the top of this file (pspmoduleinfo.h:94-95);
	 __psp_heap_blockid is libcglue's SceUID for the partition block, 0 until
	 the first _sbrk and negative if the allocation failed.
	*/
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

/*
===========================================================================
Sys_PSP_BuildBootCommandLine

Wii pattern (wii_main.c:421,424-478, Q3PORT.md 1.4): measure free memory
at boot, then inject the starting budget as "+set cvar value" boot
	 commands rather than hardcoding constants. The launch profile stays
	 comfortably
under the 31-line MAX_CONSOLE_LINES cap the Wii port hit twice.

cl_motd 0 is not a budget knob either. CL_RequestMotd (client/cl_main.c:1517)
resolves UPDATE_SERVER_NAME == update.quake3arena.com, which no longer exists,
and psp_net.c's resolver blocks the main thread for timeout x retries while it
fails. Session 9 turned that from a no-op into a real stall.

r_primitives 2 is not a budget knob and is not optional. r_primitives 0
means "auto", and R_DrawElements (renderergl1/tr_shade.c:168-175) resolves
auto to 1 - R_DrawStripElements driving qglArrayElement once per vertex -
whenever qglLockArraysEXT is NULL. There is no compiled-vertex-array
extension on the GE, so it is NULL permanently, and the Session 5 hardware
log confirmed the engine was on that path ("rendering primitives: multiple
glArrayElement"). Immediate mode is a Session 7 no-op in psp_qgl.c, so
without this line nothing draws at all no matter how correct the texture
and vertex-array paths are.

The composed launch profile must fit entirely in buf. buf[384] against a
394-byte string silently dropped the tail, so cl_motd 0 never applied and a
zero-argument set ran at boot. Both truncation points are now detected, the
compose one fatally and the append one as a logged warning.
===========================================================================
*/
void Sys_PSP_BuildBootCommandLine( char *cmdline, int size )
{
	int  heapMB     = PSP_HEAP_KB / 1024;
	int  outsideKB  = (int)( sceKernelMaxFreeMemSize() / 1024 );
	int  hunkMB;
	int  bufLen;
	char buf[ MAX_STRING_CHARS ];

	/*
	 Size the hunk from the HEAP, not from sceKernelMaxFreeMemSize().

	 Com_InitHunkMemory calloc()s the hunk, so it comes out of newlib's heap -
	 the PSP_HEAP_SIZE_KB block that libcglue claims from the user partition on
	 the first _sbrk. sceKernelMaxFreeMemSize() reports what is left OUTSIDE
	 that block, which is a different pool entirely.

	 Sizing off the wrong pool was invisible while the heap was broken (nothing
	 was ever claimed, so free memory stayed at ~39 MB and the arithmetic looked
	 sane). The moment the import-stub bug was fixed and the heap was really
	 taken, free dropped to ~3 MB and com_hunkMegs collapsed to the
	 MIN_COMHUNKMEGS floor. See Q3PORT.md 5.

	 The reserve is everything in the heap that is NOT the hunk, and textures
	 are the open-ended part of it: PSP_TexUpload2D memalign()s every mip level
	 from this same heap (code/psp/psp_tex.c:343), so whatever the reserve does
	 not spend on the fixed costs becomes the texture budget by default.

	   zone            5.0 MB   com_zoneMegs
	   sound pool      6.2 MB   com_soundMegs 2; one 3.1 MB segment is volatile
	   small zone      0.5 MB
	   newlib/stdio    ~0.5 MB  FILE objects, unzip, libjpeg workspace
	   textures        the rest (~6 MB at heap 39, hunk 24)

	 15 keeps the hunk at 24 MB across the Session 8 heap increase (36 -> 39),
	 so the texture budget comes out where Session 7 measured it working rather
	 than being silently handed to the hunk by the old reserve of 12. Cap at
	 the mirror's proven Slim value of 27 (Q3PORT.md 1.2).

	 Session 11a made this the port's tightest number: the heap ran completely
	 dry (arena 36841 KB, used 36804, free 36) and PSP_TexUpload2D started
	 failing on 256x256 uploads, while cgame reported "Memory is low. Using
	 deferred model." - which is hunk free below 4 MB (cgame/cg_players.c:1083).
	 Both pools starving at once means the split is not obviously wrong, it
	 means the total is short, and moving the boundary blind just swaps which
	 one fails. Sys_PSP_HeapReport now prints hunk free next to heap free at
	 every report point; re-tune PSP_HUNK_RESERVE_MB from those two numbers.
	*/
	hunkMB = heapMB - PSP_HUNK_RESERVE_MB;
	if( hunkMB > 27 )
		hunkMB = 27;
	if( hunkMB < MIN_COMHUNKMEGS )
		hunkMB = MIN_COMHUNKMEGS;

	/*
	 vm_cgame / vm_ui 0 == VMI_NATIVE (qcommon/qcommon.h:328). Session 10:
	 both modules are now PRX files in baseq3/ and the interpreter is a
	 fallback, not the plan - the frame breakdown put the interpreted cgame
	 at ~40% of a 124 ms frame, and there is no MIPS bytecode compiler in
	 ioquake3 to fix that any other way.

	 These are NOT archived cvars, so unlike the r_* settings above they
	 cannot be overridden by a stale q3config.cfg. If a .prx is missing,
	 FS_FindVM falls through to the .qvm on its own (files.c:1468-1497) and
	 the port keeps working - slowly - rather than failing to start.

	 Session 11 (PSP_STATIC_GAME_MODULES) makes them native without a loader;
	 the fallback is unchanged, because VM_Create's static branch drops into
	 the ordinary FS_FindVM path whenever Sys_LoadGameDll returns NULL.

	 vm_game joins them in Session 11c. The PRX build never had a qagame stub
	 group, so it stays at the interpreter there.
	*/
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
		"+set r_picmip 3 "
		"+set r_subdivisions 80 "
		"+set r_vertexlight 1 "
		/*
		 r_pspNetVblank is a launch cvar rather than an autoexec setting:
		 network play depends on the display-frame pacing path staying enabled,
		 and the launch command must win over archived config state.
		*/
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

	/*
	 Q_strcat truncates silently (q_shared.c:913-921): it errors only when
	 cmdline is already over size, never when buf does not fit in what is
	 left. Unlike the compose check above this is not fatal, because the
	 room available depends on the argv[0] path the launcher hands us, and
	 refusing to boot from a deeply nested install directory would be worse
	 than losing the trailing cvars. It must not be silent, though.
	*/
	{
		const int used = (int)strlen( cmdline );

		if( used + bufLen >= size ) {
			Sys_Print( va( "PSP: WARNING boot command line overflow, %d bytes of launch cvars dropped "
				"(cmdline %d, adding %d, capacity %d)\n",
				( used + bufLen ) - ( size - 1 ), used, bufLen, size ) );
		}
	}

	Q_strcat( cmdline, size, buf );

	/*
	 outsideKB is what remains of the user partition after the heap block was
	 taken - that, not the heap, is where Session 4's PRX modules get loaded
	 (sceKernelLoadModule allocates from the partition). If it is too small for
	 cgame/qagame/ui, lower PSP_HEAP_KB rather than the hunk.
	*/
	Sys_Print( va( "PSP: heap %d MB, outside-heap free %d KB (PRX budget), "
		"chosen hunk %d MB, zone %d MB, sound %d MB\n", heapMB, outsideKB,
		hunkMB, PSP_ZONE_MEGS, PSP_SOUND_MEGS ) );
}

/*
===========================================================================
Sys_Milliseconds

sys_unix.c's counterpart exports the globals sys_timeBase/curtime; other
engine code reads them directly. sceRtcGetCurrentTick gives microsecond
ticks since a fixed epoch - only the delta since the first call matters.
===========================================================================
*/
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

/*
===========================================================================
Sys_PlatformInit

This deliberately matches Quake3PSP-mirror/unix/unix_main.cpp:1079-1141 --
the only Quake 3 known to boot on real PSP hardware -- and NOT
DaedalusX64's SysPSP/main.cpp:79-130.

Grepping the whole mirror for kuKernelGetModel / sceKernelVolatileMemLock /
scePowerLock / sceGeEdramSetSize returns exactly one hit: a declaration at
unix/m33libs/kubridge.h:94 (there for kuKernelLoadModule, the Sys_LoadDll
fallback). The mirror calls none of them. Its main() is:
disable FP exceptions -> setupCallbacks() -> scePowerSetClockFrequency ->
engine.

The first cut of this file imported Daedalus's init wholesale and froze the
console hard on a PSP-2000. Daedalus is an emulator that genuinely needs
partition 5 and 4 MB of VRAM and earns those calls; Session 2 uses neither.
sceKernelVolatileMemLock in particular is the BLOCKING variant
(sceKernelVolatileMemTryLock is the non-blocking one) and was called at boot
and never unlocked.

Each removed call comes back at the session that actually consumes it:

  sceGeEdramSetSize(4MB) + kuKernelGetModel Slim gate
      -> Session 5, where VRAM is first allocated. Sizing the allocator
         from sceGeEdramGetSize() is the point of the call; doing it with
         no allocator is pure risk. DECISION-004's PSP-1000 refusal moves
         there with it.
  sceKernelVolatileMemLock + scePowerLock(0)
      -> Session 6, the texture overflow heap, which is what Daedalus
         actually uses partition 5 for (VideoMemoryManager.cpp:58-59).

Q3PORT.md Session 2's checklist lists the Slim init and VolatileMemInit
here; that ordering was wrong and the hardware said so.
===========================================================================
*/
void Sys_PlatformInit( void )
{
	int rc;

	PSP_SetupExitCallback();

	/*
	 The request can be refused (bad combination, power/thermal state, a CFW
	 plugin holding a clock lock) and the engine would then run the whole
	 session at 222 MHz with nothing in the log to say so - which reads
	 exactly like slow code. Read the clock back from the hardware rather
	 than trusting rc alone: a success code with a wrong clock is the case a
	 plain rc check misses. Report and continue; a slow boot beats no boot.
	*/
	rc = scePowerSetClockFrequency( 333, 333, 166 );
	PSP_TraceAppend( "PSP: clock set rc %08x -> cpu %d MHz, bus %d MHz\n",
		rc, scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency() );

	pspFpuSetEnable( 0 ); // disable FPU exceptions

	pspDebugScreenInit();
}

/*
===========================================================================
Sys_PlatformExit
===========================================================================
*/
void Sys_PlatformExit( void )
{
}

/*
===========================================================================
Sys_GLimpInit / Sys_GLimpSafeInit

NOP, same as sys_unix.c - PSP has no "safe mode" video path.
===========================================================================
*/
void Sys_GLimpSafeInit( void )
{
}

void Sys_GLimpInit( void )
{
}

/*
===========================================================================
Filesystem / path helpers

PSPSDK newlib provides POSIX fopen/opendir/readdir/stat/mkdir/getcwd over
sceIo (confirmed in use by Quake3PSP-mirror/unix/unix_shared.c and
unix_main.cpp:524's getcwd() call), so these are straightforward ports of
the sys_unix.c versions minus the XDG/dialog machinery.
===========================================================================
*/

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

/*
==================
Sys_Cwd

Deliberately NOT getcwd(): the CWD is launcher-dependent, and every caller
of this (Sys_DefaultInstallPath's fallback, FS_Startup) wants the install
directory. The resolver already tried getcwd as one candidate among
several.
==================
*/
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

// One directory for everything: there is no home directory on a PSP, and
// splitting config/data/state across three would only invent paths the user
// then has to find.
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

/*
==================
Sys_ListFilteredFiles
==================
*/
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

/*
===========================================================================
Misc
===========================================================================
*/

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

char *Sys_GetCurrentUser( void )
{
	return "player";
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
