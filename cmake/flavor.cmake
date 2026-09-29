# Game flavor. q3 (the default) is stock ioquake3 and must build exactly as before.
# oa is OpenArena 0.8.8: -DFLAVOR=oa. ta is Team Arena: -DFLAVOR=ta. Include before platforms, client and basegame.

set(FLAVOR "q3" CACHE STRING "Game flavor: q3 (Quake III Arena), ta (Team Arena) or oa (OpenArena)")
set_property(CACHE FLAVOR PROPERTY STRINGS q3 ta oa)
string(TOLOWER "${FLAVOR}" FLAVOR_LOWER)

if(FLAVOR_LOWER STREQUAL "oa" OR FLAVOR_LOWER STREQUAL "openarena")
    set(FLAVOR_ID oa)
    # Engine and game modules: q_shared.h picks the OpenArena names and baseoa.
    add_compile_definitions(STANDALONEOA)
    # OA 0.8.8 gamecode (OpenArena/gamecode, oa-0.8.8 branch), vendored as in the Xbox port.
    set(GAME_SOURCE_DIR ${CMAKE_SOURCE_DIR}/oa/code)
    set(FLAVOR_TITLE "Open Arena")
    set(FLAVOR_GRAPHICS_DIR ${CMAKE_SOURCE_DIR}/graphics/oa)
elseif(FLAVOR_LOWER STREQUAL "ta" OR FLAVOR_LOWER STREQUAL "missionpack")
    set(FLAVOR_ID ta)
    # Engine too, as in the Xbox port: botlib's gametype list gains the TA modes.
    add_compile_definitions(MISSIONPACK)
    set(GAME_SOURCE_DIR ${SOURCE_DIR})
    set(FLAVOR_TITLE "ioquake3 Team Arena")
    set(FLAVOR_GRAPHICS_DIR ${CMAKE_SOURCE_DIR}/graphics/ta)
elseif(FLAVOR_LOWER STREQUAL "q3")
    set(FLAVOR_ID q3)
    set(GAME_SOURCE_DIR ${SOURCE_DIR})
    set(FLAVOR_TITLE "ioquake3")
    set(FLAVOR_GRAPHICS_DIR ${CMAKE_SOURCE_DIR}/graphics/q3)
else()
    message(FATAL_ERROR "Unknown FLAVOR '${FLAVOR}': use q3, ta or oa")
endif()

message(STATUS "Game flavor: ${FLAVOR_ID} (${FLAVOR_TITLE})")
