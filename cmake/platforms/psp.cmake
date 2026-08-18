# PSP (Sony PSP-2000 Slim and later) specific settings
#
# Constrained-platform settings: guard-and-return, force unsupported options
# OFF via CACHE INTERNAL, and hook packaging through POST_CONFIGURE_FUNCTIONS.
#
# The pspdev CMake toolchain file ($PSPDEV/psp/share/pspdev.cmake) already
# SET(PSP TRUE), SET(PLATFORM_PSP TRUE) and add_definitions(-D__PSP__ -DPSP
# -D_PSP_FW_VERSION=600) before this file is ever included, and it also
# include()s CreatePBP.cmake + AddPrxModule.cmake. This file only adds what
# the engine build itself needs.

if(NOT PSP)
    return()
endif()

set(CMAKE_EXECUTABLE_SUFFIX ".elf")

# CMakeLists.txt:50-56 auto-enables LTO globally; PRX relocation and this
# toolchain's GCC do not get along with it. Disable it for the PSP build.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)

# Keep feature checks from trying to link executables during configuration.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ---------------------------------------------------------------------------
# Disable everything this port doesn't build yet.
# ---------------------------------------------------------------------------
set(USE_RENDERER_DLOPEN OFF CACHE INTERNAL "")
set(USE_OPENAL OFF CACHE INTERNAL "")
set(USE_OPENAL_DLOPEN OFF CACHE INTERNAL "")
set(USE_HTTP OFF CACHE INTERNAL "")
set(USE_CODEC_VORBIS OFF CACHE INTERNAL "")
set(USE_CODEC_OPUS OFF CACHE INTERNAL "")
set(USE_VOIP OFF CACHE INTERNAL "")
set(USE_MUMBLE OFF CACHE INTERNAL "")
set(USE_FREETYPE OFF CACHE INTERNAL "")
# ---------------------------------------------------------------------------
# Native game modules, static route (Session 11). This is the one that is ON.
#
# cmake/basegame.cmake builds cgame and ui into the EBOOT as two symbol-scoped
# object blobs; the block there explains the partial-link + localize mechanism
# and why plain -DvmMain=vmMainCG renaming is not enough.
#
# It flips PSP_STATIC_GAME_MODULES, which does three things:
#   code/sys/sys_psp.c      compiles the no-loader Sys_LoadGameDll
#   code/qcommon/vm.c       reaches it without FS_FindVM finding a file
#   Sys_PSP_BuildBootCommandLine  injects vm_cgame 0 / vm_ui 0
# The four must move together or the link breaks on vmMainCG/vmMainUI.
#
# The QVMs stay in the pk3s and stay reachable: every failure path in the new
# Sys_LoadGameDll returns NULL without touching *entryPoint, and vm.c falls
# through to VM_LoadQVM exactly as before.
# ---------------------------------------------------------------------------
set(BUILD_GAME_STATIC ON CACHE INTERNAL "")

# BUILD_RENDERER_GL1 stays ON: with USE_RENDERER_DLOPEN OFF,
# renderer_common.cmake FATAL_ERRORs on zero static renderers.
set(BUILD_RENDERER_GL1 ON CACHE INTERNAL "")

# ---------------------------------------------------------------------------
# Compile/link flags. -G0 is mandatory: PRX relocation breaks without it.
#
# NO --gc-sections, and no -ffunction-sections/-fdata-sections that feed it.
# create_pbp_file(BUILD_PRX) links with -Wl,-q (emit relocations) because
# psp-prxgen consumes the relocation table. Garbage-collecting sections after
# those relocs are emitted leaves dangling relocations, and the result is a
# module that will not come up. This was the Session 2 hardware failure: a
# five-variant packaging matrix (diag/psp_matrix) passed on all five, and the
# only thing the engine link had that they did not was --gc-sections.
#
# Neither hardware-proven reference uses these flags:
#   Quake3PSP-mirror/Makefile:29  LDFLAGS =            (empty)
#   daedalus Source/CMakeLists.txt:36-37  -Wall -Wextra (that is all)
#
# Q3PORT.md 1.4/4 sold them as the Wii port's free ".eh_frame win". That win
# does not exist here: measured .eh_frame in this build is 0x68 == 104 bytes,
# not the Wii's 268 KB. The unwind-table flags stay because they are
# compile-only, cost nothing and cannot affect relocations.
# ---------------------------------------------------------------------------
# -fsingle-precision-constant: unsuffixed floating literals become float
# instead of double. Without it, "x * 0.5" promotes a float expression to
# double, calls __muldf3, and truncates back - and Quake 3 is written in
# unsuffixed literals throughout. The Allegrex FPU has no double at all, so
# every one of those is software emulation.
#
# The Session 11e 4307 -> 3176 -> 441 measurements are historical comparisons;
# their original ELFs are not retained.  Do not use them as a definition of a
# "double helper".  The strict definition is __*df3 plus
# __extendsfdf2/__truncdfsf2, and the separate broad report adds every
# __*df* conversion/comparison helper. This flag reaches code/thirdparty too,
# which is safe here because libjpeg's float
# DCT wants floats and zlib is integer-only.
#
# Keep both. The header covers library CALLS (sin -> sinf), this covers
# ARITHMETIC, and neither subsumes the other.
add_compile_options(
    -O2
    -G0
    -Wall
    -ffast-math
    -fsingle-precision-constant
    -fno-strict-aliasing
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
)

# Session 13: keep the scalar Sutherland-Hodgman intersection as a build-time
# reference while benchmarking the VFPU implementation.  This is deliberately
# a configure-time choice, not a renderer cvar: the paired scalar/VFPU builds
# must differ only in the inner kernel, without a runtime dispatch in it.
option(PSP_CLIP_INTERSECT_VFPU
    "Use the VFPU implementation of PSP_ClipIntersect" ON)
if(PSP_CLIP_INTERSECT_VFPU)
    add_compile_definitions(PSP_CLIP_INTERSECT_VFPU=1)
endif()

# Session 14: keep the scalar diffuse-lighting loop as the reference while
# benchmarking a hand-written VFPU implementation.  This is deliberately a
# configure-time choice so the candidate pays no runtime dispatch cost and
# can be compared against an otherwise identical build.
option(PSP_DIFFUSE_VFPU
    "Use the VFPU implementation of RB_CalcDiffuseColor" OFF)
if(PSP_DIFFUSE_VFPU)
    add_compile_definitions(PSP_DIFFUSE_VFPU=1)
endif()

# Session 15: keep the scalar VectorArrayNormalize loop as the reference while
# benchmarking the aligned VFPU implementation.  This is deliberately a
# configure-time choice so the candidate adds no runtime dispatch to the
# renderer hot path.
option(PSP_NORMALIZE_VFPU
    "Use the VFPU implementation of VectorArrayNormalize" OFF)
if(PSP_NORMALIZE_VFPU)
    add_compile_definitions(PSP_NORMALIZE_VFPU=1)
endif()

# Session 21: allow the ceiling-matrix autoexec files to select s_khz while
# keeping the shipping launch override intact by default.  This is a
# measurement-only build hook; it does not change the normal PSP binary.
option(PSP_SESSION21_UNPIN_S_KHZ
    "Allow Session 21 autoexec files to select the PSP sound rate" OFF)
if(PSP_SESSION21_UNPIN_S_KHZ)
    add_compile_definitions(PSP_SESSION21_UNPIN_S_KHZ=1)
endif()

# Goal 22: attribute storage, sound, and network waits inside the existing
# five-second frame windows.  This is deliberately measurement-only and OFF
# by default so the normal PSP build has no trace calls or reporting.
option(PSP_FILE_TRACE
    "Trace PSP virtual-file, sound-load, and network wait stalls" OFF)
if(PSP_FILE_TRACE)
    add_compile_definitions(PSP_FILE_TRACE=1)
endif()

# Stutter investigation: extend the aggregate file trace with bounded,
# post-workload records carrying lookup and virtual-file identity.  This is
# deliberately separate from PSP_FILE_TRACE so the shipping diagnostic build
# remains byte-for-byte equivalent unless explicitly requested.
option(PSP_STUTTER_TRACE
    "Capture bounded PSP file-lookup stutter records" OFF)
if(PSP_STUTTER_TRACE)
    add_compile_definitions(PSP_FILE_TRACE=1 PSP_STUTTER_TRACE=1)
endif()

# Server browser ping-accuracy investigation: netdiag.log writes one
# open/append/close Memory Stick file per network event (master response,
# ping dispatch, getinfo reply). That per-line VFS cost turned out to
# dominate the very latency it was measuring, so it must never be in a
# normal build - OFF by default, matching PSP_FILE_TRACE's rationale exactly.
option(PSP_NET_DIAG
    "Log server browser network events to netdiag.log" OFF)
if(PSP_NET_DIAG)
    add_compile_definitions(PSP_NET_DIAG=1)
endif()

# Per-call renderer/cgame profiling is ON because queued PSP measurements
# depend on its report.  A shipping EBOOT can be configured with
# -DPSP_RENDER_PROFILE=OFF and then carries no per-call timer syscalls at all.
option(PSP_RENDER_PROFILE
    "Compile the per-call PSP renderer/cgame profiler scopes" ON)
if(PSP_RENDER_PROFILE)
    add_compile_definitions(PSP_RENDER_PROFILE=1)
endif()

# Default PSP logging policy.  CON_Print copies output into a fixed RAM buffer
# and flushes it after shutdown instead of touching the Memory Stick for every
# console line.  The matched hardware A/B run showed that this removes the
# periodic report-induced stutter without changing file activity.  Disable it
# only for a crash-focused write-through diagnostic build.
option(PSP_TRACE_DEFER_CONSOLE
    "Defer PSP console/log output to RAM until shutdown" ON)
if(PSP_TRACE_DEFER_CONSOLE)
    add_compile_definitions(PSP_TRACE_DEFER_CONSOLE=1)
endif()

# Keep PSP diagnostic reports in q3psp*.log without filling the in-game
# notify/chat area.  The reports still use the normal Com_Printf/Sys_Print
# path, so disabling this switch restores their interactive visibility for a
# profiling or bring-up run.
option(PSP_DIAGNOSTICS_LOG_ONLY
    "Keep PSP diagnostic reports in log files instead of in-game chat" ON)
if(PSP_DIAGNOSTICS_LOG_ONLY)
    add_compile_definitions(PSP_DIAGNOSTICS_LOG_ONLY=1)
endif()

# Boot instrumentation, OFF by default - configure with -DPSP_BOOT_TRACE=ON.
#
# Enables: BOOT_TRACE markers bisecting Com_Init (code/qcommon/common.c), the
# heap dump around the first malloc and Sys_PSP_ScanBss (code/sys/sys_psp.c).
# All of it goes through Sys_Print, i.e. straight to con_psp.c's write-through
# log, rather than Com_Printf which would first call CL_ConsolePrint.
#
# This is what found the import-stub bug (Q3PORT.md 5, first row) after static
# reasoning failed repeatedly. Keep it; it costs nothing when off.
option(PSP_BOOT_TRACE "PSP boot instrumentation (markers, heap dump, .bss scan)" OFF)
if(PSP_BOOT_TRACE)
    add_compile_definitions(PSP_BOOT_TRACE=1)
endif()

# TEMPORARY - Session 3 heap experiment knobs.
#
# PSP_HEAP_KB is the value handed to PSP_HEAP_SIZE_KB in code/sys/sys_psp.c.
# PSP_LOG_GEN is the generation number in the log filename, so several EBOOTs
# built from one tree cannot have their results confused with each other or
# with a leftover log. Configure with -DPSP_HEAP_KB=... -DPSP_LOG_GEN=...
# Session 8: 36864 -> 39936. Sound costs ~3.0 MB of heap that Session 7 never
# spent (see the .bss cut block in code/psp/psp_platform.h), and the same
# header frees ~2.0 MB of module image to fund it. The Session 8 hardware run
# measured 2525 KB free outside the heap with nothing using it - no PRX game
# modules, this build runs QVMs - so the remaining ~1 MB comes from there.
#
# Session 10: 39936 -> 33792. The two QVMs held 8.1 MB of HUNK on the Session
# 10 hardware run - "cgame loaded in 5773248 bytes on the hunk", "ui loaded in
# 2348512 bytes". Native PRX modules use none of it: sceKernelLoadModule
# allocates from the PARTITION, outside the newlib heap entirely.
#
# So the switch pays for itself, but only if the heap shrinks to hand that
# space to the partition. Sys_PSP_BuildBootCommandLine's hunk = heapMB - 15
# reserve means dropping 6 MB of heap moves the hunk 24 -> 18 and leaves the
# texture/zone/sound reserve untouched:
#
#   before   heap 39 MB, hunk 24 MB, outside-heap 1212 KB  (no room for a PRX)
#   after    heap 33 MB, hunk 18 MB, outside-heap ~7.2 MB  (two modules fit)
#
# 18 MB of hunk against a measured non-VM requirement of ~15.9 MB (24 minus
# the 8.1 the VMs took) leaves ~2 MB - which should also silence the five
# "Memory is low. Using deferred model." lines the 24 MB configuration
# printed, since those VMs were what crowded it.
#
# Both numbers are budget estimates, and the run reports the truth: the boot
# line prints heap/outside/hunk, and Sys_LoadGameDll prints partition free
# after each module starts. Re-tune from those, not from this comment.
#
# Session 11: 39936 -> 36864. The static modules are part of the EBOOT IMAGE,
# so their ~640 KB of text and ~2.1 MB of .bss are paid from the PARTITION at
# load time - before libcglue's _sbrk claims the heap. Session 8 measured only
# ~2.5 MB free outside the heap at 39936, which the image growth would consume
# entirely: _sbrk would then fail and EVERY malloc in the process returns NULL
# (the Session 2/3 failure mode, from a different cause). Dropping 3 MB of heap
# keeps that margin.
#
# The hunk pays for it and can afford to: hunkMB = heapMB - 15 gives 21 MB,
# against a measured non-VM requirement of ~15.9 MB (the 24 MB configuration
# minus the 8.1 MB the cgame and ui QVMs occupied - "cgame loaded in 5773248
# bytes on the hunk", "ui loaded in 2348512"). Those two are no longer on the
# hunk at all.
#
# The run reports the truth: the boot line prints heap/outside/hunk. Re-tune
# from that, not from this comment - and if outside-heap comes back tight,
# lower this number rather than the .bss caps in code/psp/psp_platform.h.
#
# Session 11c: 36864 -> 35072. qagame joined the static modules and cost
# another 1.66 MB of image (text 1,853,040 -> 2,153,160, bss 11,119,392 ->
# 12,470,200), which comes out of the partition before the heap is claimed.
# Session 11b measured 1876 KB outside the heap at boot and 1316 KB after
# sceUtilityLoadNetModule, so leaving this at 36864 would have left ~218 KB
# and made net init - not the heap - the thing that failed.
#
# The hunk stays at the proven 19 MB (34 - 15). com_soundMegs=2 is 6180 KB in
# total, but one 1536-chunk sound segment is reserved from volatile memory
# before textures begin allocating, so only the second segment comes from the
# normal heap. This keeps the original hunk/heap balance while using the
# previously untouched volatile partition.
if(NOT DEFINED PSP_HEAP_KB)
    set(PSP_HEAP_KB 35072)
endif()
if(NOT DEFINED PSP_LOG_GEN)
    set(PSP_LOG_GEN 21)
endif()
if(NOT DEFINED PSP_PERF_BUILD_ID)
    set(PSP_PERF_BUILD_ID "unlabeled" CACHE STRING
        "Build identity stamped into PSP diagnostic output")
endif()

# Everything in the heap that is not the hunk (zone + sound + textures + libc),
# in MB: com_hunkMegs is PSP_HEAP_KB/1024 minus this. Session 11a ran BOTH pools
# dry at 15 - the heap arena hit its ceiling and PSP_TexUpload2D began failing,
# while cgame reported hunk free under its 4 MB deferral threshold - so the
# split is not obviously misplaced and moving it blind only chooses which pool
# fails. Left at the proven value for the measuring run; Sys_PSP_HeapReport now
# prints hunk free beside heap free at every report point, and the next round
# sets this from those numbers.
if(NOT DEFINED PSP_HUNK_RESERVE_MB)
    set(PSP_HUNK_RESERVE_MB 15)
endif()

# Memory-pool knobs. com_soundMegs is a unit count, not literal megabytes:
# each unit allocates 1536 sndBuffer objects (~3090 KB). With the default of 2,
# one unit is placed in volatile memory and one remains in the normal heap.
# Override these with -D... when measuring a different workload.
if(NOT DEFINED PSP_SOUND_MEGS)
    set(PSP_SOUND_MEGS 2)
endif()
if(NOT DEFINED PSP_ZONE_MEGS)
    set(PSP_ZONE_MEGS 5)
endif()

add_compile_definitions(PSP_HEAP_KB=${PSP_HEAP_KB} PSP_LOG_GEN=${PSP_LOG_GEN}
    PSP_PERF_BUILD_ID="${PSP_PERF_BUILD_ID}"
    PSP_HUNK_RESERVE_MB=${PSP_HUNK_RESERVE_MB}
    PSP_SOUND_MEGS=${PSP_SOUND_MEGS} PSP_ZONE_MEGS=${PSP_ZONE_MEGS})

# The static route: the entry points are real definitions inside the blobs,
# not import stubs waiting for a loader.
if(BUILD_GAME_STATIC)
    add_compile_definitions(PSP_STATIC_GAME_MODULES)
endif()

# Memory-budget floors (DEF_COMHUNKMEGS/MIN_COMHUNKMEGS/DEF_COMZONEMEGS) live
# in code/psp/psp_platform.h and are picked up by common.c's #ifndef guards.
# Force-include it everywhere rather than threading a define through every
# target.
#
# C and C++ ONLY. Without the generator expression this is also handed to the
# assembler, which feeds a header full of C declarations to psp-as and
# produces a pile of
#
#   Error: unrecognized opcode `void Sys_PSP_ZoneBegin(int zone)'
#
# assembler invocations.
add_compile_options(
    $<$<COMPILE_LANGUAGE:C,CXX>:-include$<SEMICOLON>${SOURCE_DIR}/psp/psp_platform.h>
)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
list(APPEND SYSTEM_PLATFORM_SOURCES
    ${SOURCE_DIR}/sys/sys_psp.c
    ${SOURCE_DIR}/psp/con_psp.c
    # Virtual pk3 file handles. The PSP allows ~10 open stdio files and
    # ioquake3 holds one per pk3 for the whole process; baseq3 ships nine.
    # Quake3PSP-mirror hit this and fixed it in qcommon/ioctrl.c; here the
    # cache sits under unzip's zlib_filefunc_def instead. See psp_file.h.
    ${SOURCE_DIR}/psp/psp_file.c
    # Volatile-partition allocator (Session 11d). Linked into the engine
    # rather than the renderer because it is a memory service, not a GL one -
    # the vertex arena is the next candidate customer after textures.
    ${SOURCE_DIR}/psp/psp_pool.c
)

list(APPEND CLIENT_PLATFORM_SOURCES
    ${SOURCE_DIR}/psp/psp_input.c
    ${SOURCE_DIR}/psp/psp_snd.c
    ${SOURCE_DIR}/psp/psp_static_world.c
)

# ---------------------------------------------------------------------------
# Libraries. -lpspgum_vfpu before -lpspgu (gum depends on gu).
#
# One library is deliberately NOT here because it made the EBOOT unloadable
# on real hardware (error 8002013C) while PPSSPP ran it fine -- PPSSPP HLEs
# every module and enforces neither the user/kernel split nor residency:
#
#   pspkernel  - pulls in the *ForKernel stub set (IoFileMgrForKernel,
#                StdioForKernel, ThreadManForKernel, SysclibForKernel,
#                LoadExecForKernel, SysMemForKernel, ModuleMgrForKernel).
#                PSP_MODULE_INFO declares attr 0 == user mode, and firmware
#                refuses to load a user-mode PRX importing kernel libraries.
#                The ForUser equivalents come from pspuser/pspsdk.
#
# This block used to name pspnet* as a second cause of 8002013C, on the
# theory that importing sceNetInet before sceUtilityLoadNetModule makes the
# module unloadable. That was wrong, and Session 9 corrected it: psp-gcc's
# own spec appends -lpspnet_inet -lpspnet_resolver to EVERY link, so
# sceNetInet has been in .rodata.sceResident of every EBOOT that has ever
# run on this hardware, including the Session 2 one that passed. Imports are
# satisfied at module load from the stub table; the net PRXs being made
# resident later by sceUtilityLoadNetModule is the normal arrangement and is
# what Quake3PSP-mirror ships.
#
# pspnet / pspnet_apctl / pspwlan are the three the spec does NOT provide,
# and Session 9 (code/psp/psp_net.c) needs all three: sceNetInit/sceNetTerm,
# the sceNetApctl* association state machine, and sceWlanGetSwitchState.
#
# Verify after any change to this list:
#   psp-objdump -s -j .rodata.sceResident build-out/ioquake3.elf
# Nothing named *ForKernel may appear. sceNet, sceNetApctl, sceNetInet,
# sceNetResolver and sceWlanDrv are expected.
#
# pspkubridge (KUBridge) stays: kuKernelGetModel is how DECISION-004's Slim
# gate works, kubridge.prx is resident on ARK-4/PRO CFW, and both DaedalusX64
# (SysPSP/main.cpp:123) and the mirror import it the same way.
# ---------------------------------------------------------------------------
# pspuser, psprtc and m are deliberately NOT listed, even though this port calls
# into all three. psp-gcc's own spec already appends them, last:
#
#   -lm --start-group -lpthreadglue -lpthread -lcglue -lc --end-group
#   -lpsputility -lpsprtc -lpspnet_inet -lpspnet_resolver -lpspsdk -lpspmodinfo -lpspuser
#
# Listing them here as well gives the linker TWO scan points for the same PSP
# library. Our code pulled sceIoOpen/Close/Write/Getstat and
# sceKernelMaxFreeMemSize/GetBlockHeadAddr from the early copy; libcglue then
# pulled sceIoRead/Lseek/Dopen/... and sceKernelAllocPartitionMemory from the
# trailing spec copy. That leaves two disjoint runs of stubs for IoFileMgrForUser
# and SysMemUserForUser in .sceStub.text, and psp-fixup-imports requires every
# stub of a library to be contiguous - it gives up with
#
#   Warning: could not fixup imports, stubs out of order.
#   Ensure the SDK libraries are linked in last to correct this.
#
# The result is a module whose import table is inconsistent: the loader patches
# the first run and leaves the second unresolved. sceKernelAllocPartitionMemory
# then returns with v0 untouched, libcglue's _sbrk never gets a heap, and EVERY
# malloc in the process returns NULL. That was the Session 2/3 boot failure -
# not heap sizing, and not PSP_HEAP_SIZE_KB.
#
# Verify after any change to this list:
#   the link must be silent - no "stubs out of order" warning at all.
list(APPEND COMMON_LIBRARIES
    pspgum_vfpu
    pspvfpu
    pspfpu
    pspgu
    pspvram
    pspdisplay
    pspctrl
    pspaudio
    pspaudiolib
    psppower
    pspdebug
    pspkubridge
    pspge
    # Session 9. NOT pspnet_inet / pspnet_resolver / psputility: the spec
    # already appends those, and a second scan point for one PSP library
    # splits its stubs and triggers the fixup-imports failure described
    # below.
    pspnet
    pspnet_apctl
    pspwlan
)

# ---------------------------------------------------------------------------
# EBOOT.PBP packaging via the toolchain's own create_pbp_file() macro
# (CreatePBP.cmake, included by pspdev.cmake) instead of hand-rolled
# psp-prxgen/mksfoex/pack-pbp invocations. See cmake/post_configure.cmake for
# the POST_CONFIGURE_FUNCTIONS dispatcher and its packaging pattern.
#
# MEMSIZE 2, NOT 1. The SDK's own build.mak:43-55 is explicit:
#
#   # CFW versions after M33 3.90 guard against expanding the
#   # user memory partition on PSP-1000, making MEMSIZE obsolete.
#   # It is now an opt-out policy with PSP_LARGE_MEMORY=0
#   ifeq ($(shell test $(PSP_FW_VERSION) -gt 390; echo $$?),0)
#   EXPAND_MEMORY = 2
#   ifeq ($(PSP_LARGE_MEMORY),1)
#   $(warning "PSP_LARGE_MEMORY" flag is not necessary targeting firmware
#             versions above 3.90)
#
# We build with _PSP_FW_VERSION=600, so 2 is the correct modern value; 1 is
# the legacy 3.x path the SDK warns against. 2 is also CreatePBP.cmake's own
# default and what DaedalusX64 ships (Source/CMakeLists.txt:320-327 passes no
# MEMSIZE at all). Expanded memory still happens - via 2, not 1.
#
# Q3PORT.md DECISION-004 conflated two different mechanisms: the mirror's
# PSP_LARGE_MEMORY=1 is a build.mak variable from the 3.x era, not the
# PARAM.SFO MEMSIZE key. Setting MEMSIZE 1 here was that error propagated.
# ---------------------------------------------------------------------------
list(APPEND POST_CONFIGURE_FUNCTIONS psp_package)

# BUILD_PRX is deliberately NOT passed: a plain ELF is packed into the PBP.
#
# Established on hardware: a PRX EBOOT declaring a large .bss is rejected at
# LOAD time (black screen, main() never runs). psp-prxgen is not at fault --
# it preserves MemSiz correctly (verified: 0x425a80 for a 4 MB .bss build,
# 0xb25a98 for 11 MB). The module declares its size honestly and the loader
# refuses it. diag/psp_matrix A/B/C were plain-ELF EBOOTs and ran; D/E were
# PRX and also ran, but every variant there had a tiny .bss, so PRX-vs-ELF
# was never tested against a large one. diag/psp_matrix4 tests exactly that.
#
# Nothing in this port needs the main binary to be a PRX. The native game
# modules are linked into the EBOOT, so they are unaffected by its format.
function(psp_package)
    create_pbp_file(
        TARGET ${CLIENT_BINARY}
        TITLE "ioquake3"
        ICON_PATH "${CMAKE_SOURCE_DIR}/graphics/q3/ICON0.png"
        BACKGROUND_PATH "${CMAKE_SOURCE_DIR}/graphics/q3/PIC1.png"
        MEMSIZE 2
    )
endfunction()
