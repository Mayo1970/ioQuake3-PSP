if(NOT BUILD_GAME_STATIC)
    return()
endif()

include(utils/set_output_dirs)

if(FLAVOR_ID STREQUAL "oa")
# OA 0.8.8 lists from its Makefile, as in the Xbox port's Makefile.xbox. bg_lib.c is left
# out: it is all #ifdef Q3_VM. The modules build against OA's own qcommon, as upstream OA.
set(OA_DIR ${GAME_SOURCE_DIR})

set(CGAME_SOURCES
    ${OA_DIR}/cgame/cg_main.c
    ${OA_DIR}/game/bg_misc.c
    ${OA_DIR}/game/bg_pmove.c
    ${OA_DIR}/game/bg_slidemove.c
    ${OA_DIR}/cgame/cg_challenges.c
    ${OA_DIR}/cgame/cg_consolecmds.c
    ${OA_DIR}/cgame/cg_draw.c
    ${OA_DIR}/cgame/cg_drawtools.c
    ${OA_DIR}/cgame/cg_effects.c
    ${OA_DIR}/cgame/cg_ents.c
    ${OA_DIR}/cgame/cg_event.c
    ${OA_DIR}/cgame/cg_info.c
    ${OA_DIR}/cgame/cg_localents.c
    ${OA_DIR}/cgame/cg_marks.c
    ${OA_DIR}/cgame/cg_players.c
    ${OA_DIR}/cgame/cg_playerstate.c
    ${OA_DIR}/cgame/cg_predict.c
    ${OA_DIR}/cgame/cg_scoreboard.c
    ${OA_DIR}/cgame/cg_servercmds.c
    ${OA_DIR}/cgame/cg_snapshot.c
    ${OA_DIR}/cgame/cg_unlagged.c
    ${OA_DIR}/cgame/cg_view.c
    ${OA_DIR}/cgame/cg_weapons.c
)

set(CGAME_BINARY_SOURCES ${OA_DIR}/cgame/cg_syscalls.c)

set(GAME_SOURCES
    ${OA_DIR}/game/g_main.c
    ${OA_DIR}/game/ai_chat.c
    ${OA_DIR}/game/ai_cmd.c
    ${OA_DIR}/game/ai_dmnet.c
    ${OA_DIR}/game/ai_dmq3.c
    ${OA_DIR}/game/ai_main.c
    ${OA_DIR}/game/ai_team.c
    ${OA_DIR}/game/ai_vcmd.c
    ${OA_DIR}/game/bg_misc.c
    ${OA_DIR}/game/bg_pmove.c
    ${OA_DIR}/game/bg_slidemove.c
    ${OA_DIR}/game/g_active.c
    ${OA_DIR}/game/g_arenas.c
    ${OA_DIR}/game/g_admin.c
    ${OA_DIR}/game/g_bot.c
    ${OA_DIR}/game/g_client.c
    ${OA_DIR}/game/g_cmds.c
    ${OA_DIR}/game/g_cmds_ext.c
    ${OA_DIR}/game/g_combat.c
    ${OA_DIR}/game/g_items.c
    ${OA_DIR}/game/bg_alloc.c
    ${OA_DIR}/game/g_fileops.c
    ${OA_DIR}/game/g_killspree.c
    ${OA_DIR}/game/g_misc.c
    ${OA_DIR}/game/g_missile.c
    ${OA_DIR}/game/g_mover.c
    ${OA_DIR}/game/g_playerstore.c
    ${OA_DIR}/game/g_session.c
    ${OA_DIR}/game/g_spawn.c
    ${OA_DIR}/game/g_svcmds.c
    ${OA_DIR}/game/g_svcmds_ext.c
    ${OA_DIR}/game/g_target.c
    ${OA_DIR}/game/g_team.c
    ${OA_DIR}/game/g_trigger.c
    ${OA_DIR}/game/g_unlagged.c
    ${OA_DIR}/game/g_utils.c
    ${OA_DIR}/game/g_vote.c
    ${OA_DIR}/game/g_weapon.c
)

set(GAME_BINARY_SOURCES ${OA_DIR}/game/g_syscalls.c)

set(UI_SOURCES
    ${OA_DIR}/q3_ui/ui_main.c
    ${OA_DIR}/game/bg_misc.c
    ${OA_DIR}/q3_ui/ui_addbots.c
    ${OA_DIR}/q3_ui/ui_atoms.c
    ${OA_DIR}/q3_ui/ui_cdkey.c
    ${OA_DIR}/q3_ui/ui_challenges.c
    ${OA_DIR}/q3_ui/ui_cinematics.c
    ${OA_DIR}/q3_ui/ui_confirm.c
    ${OA_DIR}/q3_ui/ui_connect.c
    ${OA_DIR}/q3_ui/ui_controls2.c
    ${OA_DIR}/q3_ui/ui_credits.c
    ${OA_DIR}/q3_ui/ui_demo2.c
    ${OA_DIR}/q3_ui/ui_display.c
    ${OA_DIR}/q3_ui/ui_firstconnect.c
    ${OA_DIR}/q3_ui/ui_gameinfo.c
    ${OA_DIR}/q3_ui/ui_ingame.c
    ${OA_DIR}/q3_ui/ui_loadconfig.c
    ${OA_DIR}/q3_ui/ui_menu.c
    ${OA_DIR}/q3_ui/ui_mfield.c
    ${OA_DIR}/q3_ui/ui_mods.c
    ${OA_DIR}/q3_ui/ui_network.c
    ${OA_DIR}/q3_ui/ui_options.c
    ${OA_DIR}/q3_ui/ui_password.c
    ${OA_DIR}/q3_ui/ui_playermodel.c
    ${OA_DIR}/q3_ui/ui_players.c
    ${OA_DIR}/q3_ui/ui_playersettings.c
    ${OA_DIR}/q3_ui/ui_preferences.c
    ${OA_DIR}/q3_ui/ui_qmenu.c
    ${OA_DIR}/q3_ui/ui_removebots.c
    ${OA_DIR}/q3_ui/ui_saveconfig.c
    ${OA_DIR}/q3_ui/ui_serverinfo.c
    ${OA_DIR}/q3_ui/ui_servers2.c
    ${OA_DIR}/q3_ui/ui_setup.c
    ${OA_DIR}/q3_ui/ui_sound.c
    ${OA_DIR}/q3_ui/ui_sparena.c
    ${OA_DIR}/q3_ui/ui_specifyserver.c
    ${OA_DIR}/q3_ui/ui_splevel.c
    ${OA_DIR}/q3_ui/ui_sppostgame.c
    ${OA_DIR}/q3_ui/ui_spskill.c
    ${OA_DIR}/q3_ui/ui_startserver.c
    ${OA_DIR}/q3_ui/ui_team.c
    ${OA_DIR}/q3_ui/ui_teamorders.c
    ${OA_DIR}/q3_ui/ui_video.c
    ${OA_DIR}/q3_ui/ui_votemenu.c
    ${OA_DIR}/q3_ui/ui_votemenu_fraglimit.c
    ${OA_DIR}/q3_ui/ui_votemenu_timelimit.c
    ${OA_DIR}/q3_ui/ui_votemenu_gametype.c
    ${OA_DIR}/q3_ui/ui_votemenu_kick.c
    ${OA_DIR}/q3_ui/ui_votemenu_map.c
    ${OA_DIR}/q3_ui/ui_votemenu_custom.c
)

set(UI_BINARY_SOURCES ${OA_DIR}/ui/ui_syscalls.c)

set(GAME_MODULE_SHARED_SOURCES
    ${OA_DIR}/qcommon/q_math.c
    ${OA_DIR}/qcommon/q_shared.c
)
else()
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

if(FLAVOR_ID STREQUAL "ta")
# Team Arena lists from upstream cmake/missionpack.cmake: cgame adds the scripted HUD, ui is the
# menu-script ui. qagame is the baseq3 list; MISSIONPACK (flavor.cmake) selects its TA code.
list(APPEND CGAME_SOURCES
    ${SOURCE_DIR}/cgame/cg_newdraw.c
    ${SOURCE_DIR}/ui/ui_shared.c
)

set(UI_SOURCES
    ${SOURCE_DIR}/ui/ui_main.c
    ${SOURCE_DIR}/ui/ui_atoms.c
    ${SOURCE_DIR}/ui/ui_gameinfo.c
    ${SOURCE_DIR}/ui/ui_players.c
    ${SOURCE_DIR}/ui/ui_shared.c
    ${SOURCE_DIR}/game/bg_misc.c
    ${SOURCE_DIR}/game/bg_lib.c
)
endif()

set(UI_BINARY_SOURCES ${SOURCE_DIR}/ui/ui_syscalls.c)

set(GAME_MODULE_SHARED_SOURCES
    ${SOURCE_DIR}/qcommon/q_math.c
    ${SOURCE_DIR}/qcommon/q_shared.c
)
endif()

set(CGAME_SOURCES_BASEGAME ${CGAME_SOURCES} ${GAME_MODULE_SHARED_SOURCES})
set(GAME_SOURCES_BASEGAME ${GAME_SOURCES} ${GAME_MODULE_SHARED_SOURCES})
set(UI_SOURCES_BASEGAME ${UI_SOURCES} ${GAME_MODULE_SHARED_SOURCES})

# PSP: cgame, ui and qagame link into the EBOOT (a PRX EBOOT with this .bss is refused at load).
# Each module is partial-linked and localized to its two entry points, so shared symbols never clash.
if(BUILD_GAME_STATIC)
    if(NOT CMAKE_LINKER)
        find_program(CMAKE_LINKER psp-ld REQUIRED)
    endif()
    if(NOT CMAKE_OBJCOPY)
        find_program(CMAKE_OBJCOPY psp-objcopy REQUIRED)
    endif()

    # name: target ("cgame"); suffix: entry-point suffix ("CG") as declared in code/sys/sys_psp.c;
    # define: the module's own build define (CGAME, UI, QAGAME).
    function(add_static_game_module name suffix define)
        set(objs ${name}_objs)
        set(merged ${CMAKE_CURRENT_BINARY_DIR}/${name}_merged.o)
        set(blob   ${CMAKE_CURRENT_BINARY_DIR}/${name}_static.o)

        add_library(${objs} OBJECT ${ARGN})
        target_compile_definitions(${objs} PRIVATE ${define})

        add_custom_command(OUTPUT ${blob}
            # -r: partial link into one object. -d: allocate COMMON symbols now; objcopy cannot localize a
            # common symbol, and it would collide in the engine link.
            COMMAND ${CMAKE_LINKER} -r -d -o ${merged} $<TARGET_OBJECTS:${objs}>
            # Keep only the two entry points global (undefined libc refs still resolve); the renamed sections
            # get __start_/__stop_ symbols, so Sys_LoadGameDll can restore .data and zero .bss on each load.
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

