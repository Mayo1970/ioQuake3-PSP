# PSP (PSP-2000 Slim and later). pspdev.cmake already defines PSP/PLATFORM_PSP, -D__PSP__
# and includes CreatePBP.cmake; this file adds only what the engine build needs.

if(NOT PSP)
    return()
endif()

set(CMAKE_EXECUTABLE_SUFFIX ".elf")

# CMakeLists.txt enables LTO globally; PRX relocation and this GCC do not get along with it.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)

# Keep feature checks from trying to link executables during configuration.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# --- Disable everything this port doesn't build yet ---
set(USE_RENDERER_DLOPEN OFF CACHE INTERNAL "")
set(USE_OPENAL OFF CACHE INTERNAL "")
set(USE_OPENAL_DLOPEN OFF CACHE INTERNAL "")
set(USE_HTTP OFF CACHE INTERNAL "")
set(USE_CODEC_VORBIS OFF CACHE INTERNAL "")
set(USE_CODEC_OPUS OFF CACHE INTERNAL "")
# OA music is Ogg Vorbis; Q3 ships WAV only. pspdev's Tremor (integer), see snd_codec_ogg.c.
if(FLAVOR_ID STREQUAL "oa")
    list(APPEND CLIENT_DEFINITIONS USE_CODEC_VORBIS)
    list(APPEND CLIENT_LIBRARIES vorbisidec ogg)
endif()
set(USE_VOIP OFF CACHE INTERNAL "")
set(USE_MUMBLE OFF CACHE INTERNAL "")
set(USE_FREETYPE OFF CACHE INTERNAL "")
# Native game modules, static route: basegame.cmake links cgame, ui and qagame into the EBOOT.
# This flips PSP_STATIC_GAME_MODULES; the QVMs in the pk3s stay reachable as the fallback.
set(BUILD_GAME_STATIC ON CACHE INTERNAL "")

# With USE_RENDERER_DLOPEN OFF, renderer_common.cmake needs exactly one static renderer.
set(BUILD_RENDERER_GL1 ON CACHE INTERNAL "")

# -G0 is mandatory; no --gc-sections, it leaves the -Wl,-q relocations dangling (Session 2 failure).
# -fsingle-precision-constant keeps unsuffixed literals off soft-float double; the float remap header covers calls.
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

# Session 13: VFPU vs scalar Sutherland-Hodgman intersection, a configure-time choice
# so the paired builds differ only in the kernel.
option(PSP_CLIP_INTERSECT_VFPU
    "Use the VFPU implementation of PSP_ClipIntersect" ON)
if(PSP_CLIP_INTERSECT_VFPU)
    add_compile_definitions(PSP_CLIP_INTERSECT_VFPU=1)
endif()

# Session 14: VFPU RB_CalcDiffuseColor candidate; configure-time so it adds no runtime dispatch.
option(PSP_DIFFUSE_VFPU
    "Use the VFPU implementation of RB_CalcDiffuseColor" OFF)
if(PSP_DIFFUSE_VFPU)
    add_compile_definitions(PSP_DIFFUSE_VFPU=1)
endif()

# Session 15: VFPU VectorArrayNormalize candidate; configure-time for the same reason.
option(PSP_NORMALIZE_VFPU
    "Use the VFPU implementation of VectorArrayNormalize" OFF)
if(PSP_NORMALIZE_VFPU)
    add_compile_definitions(PSP_NORMALIZE_VFPU=1)
endif()

# Session 21 measurement hook: lets autoexec files pick s_khz. It does not change a normal build.
option(PSP_SESSION21_UNPIN_S_KHZ
    "Allow Session 21 autoexec files to select the PSP sound rate" OFF)
if(PSP_SESSION21_UNPIN_S_KHZ)
    add_compile_definitions(PSP_SESSION21_UNPIN_S_KHZ=1)
endif()

# Goal 22: storage, sound and network wait attribution. Measurement only, OFF by default.
option(PSP_FILE_TRACE
    "Trace PSP virtual-file, sound-load, and network wait stalls" OFF)
if(PSP_FILE_TRACE)
    add_compile_definitions(PSP_FILE_TRACE=1)
endif()

# Adds bounded per-lookup stutter records on top of PSP_FILE_TRACE. Separate, so the shipping
# diagnostic build stays byte-identical unless asked for.
option(PSP_STUTTER_TRACE
    "Capture bounded PSP file-lookup stutter records" OFF)
if(PSP_STUTTER_TRACE)
    add_compile_definitions(PSP_FILE_TRACE=1 PSP_STUTTER_TRACE=1)
endif()

# netdiag.log writes one Memory Stick file per network event, and that cost dominated the
# latency it measured. Never in a normal build.
option(PSP_NET_DIAG
    "Log server browser network events to netdiag.log" OFF)
if(PSP_NET_DIAG)
    add_compile_definitions(PSP_NET_DIAG=1)
endif()

# ON because queued measurements read its report; -DPSP_RENDER_PROFILE=OFF removes every
# per-call timer syscall from a shipping EBOOT.
option(PSP_RENDER_PROFILE
    "Compile the per-call PSP renderer/cgame profiler scopes" ON)
if(PSP_RENDER_PROFILE)
    add_compile_definitions(PSP_RENDER_PROFILE=1)
endif()

# Console output goes to a RAM buffer flushed at shutdown; the matched A/B removed the
# report-induced stutter. Turn off only for a crash-focused write-through build.
option(PSP_TRACE_DEFER_CONSOLE
    "Defer PSP console/log output to RAM until shutdown" ON)
if(PSP_TRACE_DEFER_CONSOLE)
    add_compile_definitions(PSP_TRACE_DEFER_CONSOLE=1)
endif()

# Diagnostic reports stay in q3psp*.log instead of the notify/chat area.
option(PSP_DIAGNOSTICS_LOG_ONLY
    "Keep PSP diagnostic reports in log files instead of in-game chat" ON)
if(PSP_DIAGNOSTICS_LOG_ONLY)
    add_compile_definitions(PSP_DIAGNOSTICS_LOG_ONLY=1)
endif()

# Com_Init markers, the first-malloc heap dump and Sys_PSP_ScanBss, via write-through Sys_Print.
# It found the import-stub bug after static reasoning failed; it costs nothing when off.
option(PSP_BOOT_TRACE "PSP boot instrumentation (markers, heap dump, .bss scan)" OFF)
if(PSP_BOOT_TRACE)
    add_compile_definitions(PSP_BOOT_TRACE=1)
endif()

# Xbox-port memory work: cinematic state only while playing, image decodes heap-first, in-place uploads.
# ON for every flavor; -DPSP_XBOX_MEMORY=OFF gives the previous hardware-tested layout for A/B runs.
if(NOT DEFINED PSP_XBOX_MEMORY)
    set(PSP_XBOX_MEMORY ON)
endif()
if(PSP_XBOX_MEMORY)
    add_compile_definitions(PSP_XBOX_MEMORY)
endif()

# PSP_HEAP_KB feeds PSP_HEAP_SIZE_KB. The other heaps take their EBOOT image savings against the
# tested 35072 KB q3 build less 68-136 KB, so the outside-heap PRX budget (1313 KB) keeps a margin.
if(NOT DEFINED PSP_HEAP_KB)
    if(FLAVOR_ID STREQUAL "oa" AND PSP_XBOX_MEMORY)
        set(PSP_HEAP_KB 37440)
    elseif(FLAVOR_ID STREQUAL "oa")
        set(PSP_HEAP_KB 34432)
    elseif(FLAVOR_ID STREQUAL "ta" AND PSP_XBOX_MEMORY)
        set(PSP_HEAP_KB 35264)
    elseif(FLAVOR_ID STREQUAL "ta")
        set(PSP_HEAP_KB 32256)
    elseif(PSP_XBOX_MEMORY)
        set(PSP_HEAP_KB 38016)
    else()
        set(PSP_HEAP_KB 35072)
    endif()
endif()
# PSP_LOG_GEN numbers the log file, so logs from different EBOOTs cannot be confused.
if(NOT DEFINED PSP_LOG_GEN)
    set(PSP_LOG_GEN 22)
endif()
if(NOT DEFINED PSP_PERF_BUILD_ID)
    set(PSP_PERF_BUILD_ID "unlabeled" CACHE STRING
        "Build identity stamped into PSP diagnostic output")
endif()

# Heap MB that is not hunk (zone, vertex arena, display list, libc, texture spill). It keeps the
# tested 22 MB hunk, so a larger heap goes to decodes and spill (12 for the 35072 KB heap).
if(NOT DEFINED PSP_HUNK_RESERVE_MB)
    math(EXPR PSP_HUNK_RESERVE_MB "${PSP_HEAP_KB} / 1024 - 22")
endif()

# com_soundMegs is a unit count: 1536 sndBuffers, ~3090 KB, reserved in volatile memory.
# One ADPCM unit holds twice what two 16-bit units did.
if(NOT DEFINED PSP_SOUND_MEGS)
    set(PSP_SOUND_MEGS 1)
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

# Budget floors and PSP declarations live in psp_platform.h, force-included everywhere.
# C and C++ only: handed to psp-as, the C declarations fail as "unrecognized opcode".
add_compile_options(
    $<$<COMPILE_LANGUAGE:C,CXX>:-include$<SEMICOLON>${SOURCE_DIR}/psp/psp_platform.h>
)

# --- Sources ---
list(APPEND SYSTEM_PLATFORM_SOURCES
    ${SOURCE_DIR}/sys/sys_psp.c
    ${SOURCE_DIR}/psp/con_psp.c
    # Virtual pk3 handles: the PSP allows ~10 open stdio files and baseq3 alone holds nine.
    ${SOURCE_DIR}/psp/psp_file.c
    # Volatile-partition allocator; engine-side because it is a memory service, not a GL one.
    ${SOURCE_DIR}/psp/psp_pool.c
)

list(APPEND CLIENT_PLATFORM_SOURCES
    ${SOURCE_DIR}/psp/psp_input.c
    ${SOURCE_DIR}/psp/psp_snd.c
    ${SOURCE_DIR}/psp/psp_adpcm.c
    ${SOURCE_DIR}/psp/psp_static_world.c
)

# --- Libraries, gum before gu. No pspkernel: *ForKernel stubs make the EBOOT unloadable (8002013C) ---
# No m/pspuser/psprtc/psputility/pspnet_inet/pspnet_resolver: the spec appends them, and a second copy splits their stubs.
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
    # The three net libraries the spec does not provide: sceNet*, sceNetApctl*, sceWlan*.
    pspnet
    pspnet_apctl
    pspwlan
)

# --- EBOOT.PBP through the toolchain's create_pbp_file(); post_configure.cmake dispatches it ---
# MEMSIZE 1 = the 52 MB user partition. ARK 5 treats 2 as Vita-style 24 MB and the heap fails.
list(APPEND POST_CONFIGURE_FUNCTIONS psp_package)

# No BUILD_PRX: a PRX EBOOT with a large .bss is refused at load (black screen). Art comes from
# graphics/<flavor>/; a flavor without ICON0.png or PIC1.png builds without it.
function(psp_package)
    set(PSP_PBP_ART)
    if(EXISTS "${FLAVOR_GRAPHICS_DIR}/ICON0.png")
        list(APPEND PSP_PBP_ART ICON_PATH "${FLAVOR_GRAPHICS_DIR}/ICON0.png")
    endif()
    if(EXISTS "${FLAVOR_GRAPHICS_DIR}/PIC1.png")
        list(APPEND PSP_PBP_ART BACKGROUND_PATH "${FLAVOR_GRAPHICS_DIR}/PIC1.png")
    endif()
    create_pbp_file(
        TARGET ${CLIENT_BINARY}
        TITLE "${FLAVOR_TITLE}"
        ${PSP_PBP_ART}
        MEMSIZE 1
    )
endfunction()
