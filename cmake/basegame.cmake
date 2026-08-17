if(NOT BUILD_GAME_STATIC)
    return()
endif()

include(utils/set_output_dirs)

set(CGAME_SOURCES
    ${SOURCE_DIR}/cgame/cg_main.c
    ${SOURCE_DIR}/game/bg_misc.c
    ${SOURCE_DIR}/game/bg_pmove.c
    ${SOURCE_DIR}/game/bg_slidemove.c
    ${SOURCE_DIR}/game/bg_lib.c
    ${SOURCE_DIR}/cgame/cg_consolecmds.c
    ${SOURCE_DIR}/cgame/cg_draw.c
    ${SOURCE_DIR}/cgame/cg_drawtools.c
    ${SOURCE_DIR}/cgame/cg_effects.c
    ${SOURCE_DIR}/cgame/cg_ents.c
    ${SOURCE_DIR}/cgame/cg_event.c
    ${SOURCE_DIR}/cgame/cg_info.c
    ${SOURCE_DIR}/cgame/cg_localents.c
    ${SOURCE_DIR}/cgame/cg_marks.c
    ${SOURCE_DIR}/cgame/cg_particles.c
    ${SOURCE_DIR}/cgame/cg_players.c
    ${SOURCE_DIR}/cgame/cg_playerstate.c
    ${SOURCE_DIR}/cgame/cg_predict.c
    ${SOURCE_DIR}/cgame/cg_scoreboard.c
    ${SOURCE_DIR}/cgame/cg_servercmds.c
    ${SOURCE_DIR}/cgame/cg_snapshot.c
    ${SOURCE_DIR}/cgame/cg_view.c
    ${SOURCE_DIR}/cgame/cg_weapons.c
)

set(CGAME_BINARY_SOURCES ${SOURCE_DIR}/cgame/cg_syscalls.c)

set(GAME_SOURCES
    ${SOURCE_DIR}/game/g_main.c
    ${SOURCE_DIR}/game/ai_chat.c
    ${SOURCE_DIR}/game/ai_cmd.c
    ${SOURCE_DIR}/game/ai_dmnet.c
    ${SOURCE_DIR}/game/ai_dmq3.c
    ${SOURCE_DIR}/game/ai_main.c
    ${SOURCE_DIR}/game/ai_team.c
    ${SOURCE_DIR}/game/ai_vcmd.c
    ${SOURCE_DIR}/game/bg_misc.c
    ${SOURCE_DIR}/game/bg_pmove.c
    ${SOURCE_DIR}/game/bg_slidemove.c
    ${SOURCE_DIR}/game/bg_lib.c
    ${SOURCE_DIR}/game/g_active.c
    ${SOURCE_DIR}/game/g_arenas.c
    ${SOURCE_DIR}/game/g_bot.c
    ${SOURCE_DIR}/game/g_client.c
    ${SOURCE_DIR}/game/g_cmds.c
    ${SOURCE_DIR}/game/g_combat.c
    ${SOURCE_DIR}/game/g_items.c
    ${SOURCE_DIR}/game/g_mem.c
    ${SOURCE_DIR}/game/g_misc.c
    ${SOURCE_DIR}/game/g_missile.c
    ${SOURCE_DIR}/game/g_mover.c
    ${SOURCE_DIR}/game/g_session.c
    ${SOURCE_DIR}/game/g_spawn.c
    ${SOURCE_DIR}/game/g_svcmds.c
    ${SOURCE_DIR}/game/g_target.c
    ${SOURCE_DIR}/game/g_team.c
    ${SOURCE_DIR}/game/g_trigger.c
    ${SOURCE_DIR}/game/g_utils.c
    ${SOURCE_DIR}/game/g_weapon.c
)

set(GAME_BINARY_SOURCES ${SOURCE_DIR}/game/g_syscalls.c)

set(UI_SOURCES
    ${SOURCE_DIR}/q3_ui/ui_main.c
    ${SOURCE_DIR}/game/bg_misc.c
    ${SOURCE_DIR}/game/bg_lib.c
    ${SOURCE_DIR}/q3_ui/ui_addbots.c
    ${SOURCE_DIR}/q3_ui/ui_atoms.c
    ${SOURCE_DIR}/q3_ui/ui_cdkey.c
    ${SOURCE_DIR}/q3_ui/ui_cinematics.c
    ${SOURCE_DIR}/q3_ui/ui_confirm.c
    ${SOURCE_DIR}/q3_ui/ui_connect.c
    ${SOURCE_DIR}/q3_ui/ui_controls2.c
    ${SOURCE_DIR}/q3_ui/ui_credits.c
    ${SOURCE_DIR}/q3_ui/ui_demo2.c
    ${SOURCE_DIR}/q3_ui/ui_display.c
    ${SOURCE_DIR}/q3_ui/ui_gameinfo.c
    ${SOURCE_DIR}/q3_ui/ui_ingame.c
    ${SOURCE_DIR}/q3_ui/ui_loadconfig.c
    ${SOURCE_DIR}/q3_ui/ui_menu.c
    ${SOURCE_DIR}/q3_ui/ui_mfield.c
    ${SOURCE_DIR}/q3_ui/ui_mods.c
    ${SOURCE_DIR}/q3_ui/ui_network.c
    ${SOURCE_DIR}/q3_ui/ui_options.c
    ${SOURCE_DIR}/q3_ui/ui_playermodel.c
    ${SOURCE_DIR}/q3_ui/ui_players.c
    ${SOURCE_DIR}/q3_ui/ui_playersettings.c
    ${SOURCE_DIR}/q3_ui/ui_preferences.c
    ${SOURCE_DIR}/q3_ui/ui_qmenu.c
    ${SOURCE_DIR}/q3_ui/ui_removebots.c
    ${SOURCE_DIR}/q3_ui/ui_saveconfig.c
    ${SOURCE_DIR}/q3_ui/ui_serverinfo.c
    ${SOURCE_DIR}/q3_ui/ui_servers2.c
    ${SOURCE_DIR}/q3_ui/ui_setup.c
    ${SOURCE_DIR}/q3_ui/ui_sound.c
    ${SOURCE_DIR}/q3_ui/ui_sparena.c
    ${SOURCE_DIR}/q3_ui/ui_specifyserver.c
    ${SOURCE_DIR}/q3_ui/ui_splevel.c
    ${SOURCE_DIR}/q3_ui/ui_sppostgame.c
    ${SOURCE_DIR}/q3_ui/ui_spskill.c
    ${SOURCE_DIR}/q3_ui/ui_startserver.c
    ${SOURCE_DIR}/q3_ui/ui_team.c
    ${SOURCE_DIR}/q3_ui/ui_teamorders.c
    ${SOURCE_DIR}/q3_ui/ui_video.c
)

set(UI_BINARY_SOURCES ${SOURCE_DIR}/ui/ui_syscalls.c)

set(GAME_MODULE_SHARED_SOURCES
    ${SOURCE_DIR}/qcommon/q_math.c
    ${SOURCE_DIR}/qcommon/q_shared.c
)

set(CGAME_SOURCES_BASEGAME ${CGAME_SOURCES} ${GAME_MODULE_SHARED_SOURCES})
set(GAME_SOURCES_BASEGAME ${GAME_SOURCES} ${GAME_MODULE_SHARED_SOURCES})
set(UI_SOURCES_BASEGAME ${UI_SOURCES} ${GAME_MODULE_SHARED_SOURCES})

# ---------------------------------------------------------------------------
# PSP: native game modules linked STRAIGHT INTO THE EBOOT (Session 11).
#
# Why this route: runtime loading of an unsigned PRX game module
# needs the host EBOOT to be a PRX itself (Quake3PSP-mirror Makefile:8), and a
# PRX EBOOT declaring a large .bss is refused at LOAD time on hardware - this
# port's .bss is 8.6 MB. Session 10 hit exactly that as 0x80020148. Linking the
# modules in removes the loader from the picture, so the gate does not apply.
# The mirror sketches this route at unix/unix_main.cpp:620-648.
#
# THE WORK HERE IS DUPLICATE SYMBOLS, NOT LOADING. In one static link:
#
#   vmMain / dllEntry      cg_main.c:46, cg_syscalls.c:34,
#                          ui_main.c:43, ui_syscalls.c:33
#   Com_Printf / Com_Error cg_main.c:445,456, ui_atoms.c:33,44, AND the
#                          engine's own qcommon/common.c
#   ~150 trap_*            cg_syscalls.c and ui_syscalls.c, identical names
#   bg_misc / q_shared /   compiled into the cgame set, the ui set and the
#   q_math                 engine
#
# So -DvmMain=vmMainCG renaming (which is all the mirror's sketch needed and
# never actually implemented) covers two symbols out of hundreds.
#
# The fix is per-module symbol scope, applied to the linked result rather than
# the sources: partial-link each module's objects into ONE object file, then
# make every defined global in it local EXCEPT its two entry points. Each blob
# then satisfies itself internally - its own q_shared, its own bg_misc, its own
# Com_Printf - and offers the engine exactly vmMainCG/dllEntryCG (or the UI
# pair). Nothing about the module sources changes.
#
# bg_lib.c needs no special handling: the whole file sits inside #ifdef Q3_VM
# and compiles to nothing for a native build.
#
# All three modules are included. qagame joined in Session 11c: with cgame
# native, the listen server's interpreted g_active/bot-AI tick showed up as
# svFrame at 15-24% of the frame, and dropping its QVM also returns 4.33 MB of
# hunk (measured: "qagame loaded in 4431232 bytes on the hunk").
# ---------------------------------------------------------------------------
if(BUILD_GAME_STATIC)
    if(NOT CMAKE_LINKER)
        find_program(CMAKE_LINKER psp-ld REQUIRED)
    endif()
    if(NOT CMAKE_OBJCOPY)
        find_program(CMAKE_OBJCOPY psp-objcopy REQUIRED)
    endif()

    # name   - target/base name ("cgame")
    # suffix - entry-point suffix ("CG"), matching the extern declarations in
    #          code/sys/sys_psp.c
    # define - CGAME / UI, the module's own build define
    function(add_static_game_module name suffix define)
        set(objs ${name}_objs)
        set(merged ${CMAKE_CURRENT_BINARY_DIR}/${name}_merged.o)
        set(blob   ${CMAKE_CURRENT_BINARY_DIR}/${name}_static.o)

        add_library(${objs} OBJECT ${ARGN})
        target_compile_definitions(${objs} PRIVATE ${define})

        add_custom_command(OUTPUT ${blob}
            # -r: partial link, keep relocations, produce one object.
            # -d: assign space to COMMON symbols now. Without it they stay
            #     common, and objcopy cannot localize a common symbol - the
            #     module's uninitialised globals would leak into the engine
            #     link and collide there.
            COMMAND ${CMAKE_LINKER} -r -d -o ${merged} $<TARGET_OBJECTS:${objs}>
            # --keep-global-symbol localizes every other DEFINED global and
            # leaves undefined ones alone (binutils filter_symbols guards
            # localization with !undefined), so the blob's references to
            # newlib/libc still resolve at the final link.
            #
            # The section renames give the module's mutable state a name the
            # engine can find. ld emits __start_<name>/__stop_<name> for any
            # orphan section whose name is a valid C identifier, so
            # "cgame_data"/"cgame_bss" are addressable from sys_psp.c while
            # ".data"/".bss" (dots) would not be. Sys_LoadGameDll uses them to
            # restore pristine .data and zero .bss on every reload, which is
            # what a dlopen or a QVM load does for free and a static link does
            # not - see the state-reset note there.
            COMMAND ${CMAKE_OBJCOPY}
                --redefine-sym vmMain=vmMain${suffix}
                --redefine-sym dllEntry=dllEntry${suffix}
                --keep-global-symbol=vmMain${suffix}
                --keep-global-symbol=dllEntry${suffix}
                --rename-section .data=${name}_data
                --rename-section .bss=${name}_bss
                ${merged} ${blob}
            DEPENDS ${objs} $<TARGET_OBJECTS:${objs}>
            COMMAND_EXPAND_LISTS
            VERBATIM
            COMMENT "Localizing ${name} into ${name}_static.o")

        add_custom_target(${name}_blob DEPENDS ${blob})

        # client.cmake (CMakeLists.txt:107) runs before this file (:108), so
        # the client target already exists and can just be given the object.
        set_source_files_properties(${blob} PROPERTIES
            EXTERNAL_OBJECT TRUE
            GENERATED TRUE)
        target_sources(${CLIENT_BINARY} PRIVATE ${blob})
        add_dependencies(${CLIENT_BINARY} ${name}_blob)
    endfunction()

    add_static_game_module(cgame CG CGAME
        ${CGAME_SOURCES_BASEGAME} ${CGAME_BINARY_SOURCES})

    add_static_game_module(ui UI UI
        ${UI_SOURCES_BASEGAME} ${UI_BINARY_SOURCES})

    add_static_game_module(qagame QAG QAGAME
        ${GAME_SOURCES_BASEGAME} ${GAME_BINARY_SOURCES})
endif()

