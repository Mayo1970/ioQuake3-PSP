include_guard(GLOBAL)

set(RENDERER_COMMON_SOURCES
    ${SOURCE_DIR}/renderercommon/tr_font.c
    ${SOURCE_DIR}/renderercommon/tr_image_bmp.c
    ${SOURCE_DIR}/renderercommon/tr_image_jpg.c
    ${SOURCE_DIR}/renderercommon/tr_image_pcx.c
    ${SOURCE_DIR}/renderercommon/tr_image_png.c
    ${SOURCE_DIR}/renderercommon/tr_image_pvr.c
    ${SOURCE_DIR}/renderercommon/tr_image_tga.c
    ${SOURCE_DIR}/renderercommon/tr_noise.c
    ${SOURCE_DIR}/renderercommon/puff.c
)

# PSP renderer sources: psp_glimp.c owns the display (VRAM buffers, sceGuInit,
# swap); psp_qgl.c owns the qgl* vtable that translates GL to sceGu, forwarding
# to psp_tex.c (texture objects, 16-bit conversion, swizzle, cache) and
# psp_draw.c (GL vertex arrays -> one interleaved sceGuDrawArray). All are
# renderer sources because they define symbols renderergl1 links against.
#
# psp_tcmod.c (Session 12b) is the exception that proves that rule: it is
# called BY tr_shade.c rather than by the qgl table, and it is the only file
# here that includes renderergl1/tr_local.h - it needs tess, the shader stage
# layout and tr.sinTable. It folds a stage's affine tcMod chain into one GE
# texture matrix instead of running it per vertex.
set(SDL_RENDERER_SOURCES
    ${SOURCE_DIR}/psp/psp_glimp.c
    ${SOURCE_DIR}/psp/psp_qgl.c
    ${SOURCE_DIR}/psp/psp_tex.c
    ${SOURCE_DIR}/psp/psp_draw.c
    ${SOURCE_DIR}/psp/psp_tcmod.c
    ${SOURCE_DIR}/psp/psp_gamma.c
)

set(DYNAMIC_RENDERER_SOURCES
    ${SOURCE_DIR}/renderercommon/tr_subs.c
    ${SOURCE_DIR}/qcommon/q_shared.c
    ${SOURCE_DIR}/qcommon/q_math.c
)

if(USE_FREETYPE)
    list(APPEND RENDERER_DEFINITIONS BUILD_FREETYPE)
endif()

if(USE_RENDERER_DLOPEN)
    list(APPEND RENDERER_DEFINITIONS USE_RENDERER_DLOPEN)
elseif(BUILD_RENDERER_GL1 AND BUILD_RENDERER_GL2)
    message(FATAL_ERROR "Multiple static renderers enabled; choose one")
elseif(NOT BUILD_RENDERER_GL1 AND NOT BUILD_RENDERER_GL2)
    message(FATAL_ERROR "Zero static renderers enabled; choose one")
endif()

list(APPEND RENDERER_LIBRARIES ${COMMON_LIBRARIES})
