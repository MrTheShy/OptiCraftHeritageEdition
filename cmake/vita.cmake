include(${CMAKE_SOURCE_DIR}/cmake/SourceSelection.cmake)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "OptiCraft")
set(VITA_TITLEID "OPTC00001")
set(VITA_VERSION "01.00")
set(VITA_DEPS "${CMAKE_SOURCE_DIR}/vita-deps")

set(MC_LOG_LEVEL "0" CACHE STRING "Unified diagnostic verbosity: 0=off, 1=info, 2=debug, 3=trace")
set_property(CACHE MC_LOG_LEVEL PROPERTY STRINGS 0 1 2 3)

opticraft_collect_platform_sources(VITA_SOURCES pc)
opticraft_exclude_sources(VITA_SOURCES
    "[/\\]pc[/\\]win32[/\\]"
    "[/\\]pc[/\\]render[/\\]d3d9[/\\]"
    "[/\\]pc[/\\]external[/\\]miniaudio\\.cpp$"
)

set(VITA_MINIZIP_SOURCES
    "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip/ioapi.c"
    "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip/unzip.c"
)
set_source_files_properties(${VITA_MINIZIP_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS USE_FILE32API
)
list(APPEND VITA_SOURCES ${VITA_MINIZIP_SOURCES})

opticraft_select_platform_backends(VITA_SOURCES PC PC PC)
option(VITA_PROFILER "Per-phase profiler in debug.log (needs MC_LOG_LEVEL > 0)" ON)
if(MC_LOG_LEVEL GREATER 0 AND VITA_PROFILER)
    # The Wii profiler is portable: every 10 s it logs frame/tick/render/swap
    # times and per-phase costs to debug.log. The lines keep the "wii.perf"
    # tag on the Vita: the backend is shared with the Wii build.
    list(TRANSFORM VITA_SOURCES REPLACE "(Profiler|ClientProfilerBackend)_PC\\.cpp$" "\\1_WII.cpp")
    set(VITA_PROFILE_DEFINITIONS PLATFORM_PROFILE_RENDER_PHASES=1 PLATFORM_PROFILE_STREAMING=1)
endif()

set_source_files_properties(${FDLIBM_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS __LITTLE_ENDIAN
)

add_executable(OptiCraft ${VITA_SOURCES})

set_target_properties(OptiCraft PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

target_compile_options(OptiCraft PRIVATE
    -O3
    -mtune=cortex-a9
    -ffunction-sections
    -fdata-sections
    -fno-math-errno
    -fno-trapping-math
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

target_compile_definitions(OptiCraft PRIVATE
    MC_LINUX
    VITA_PLATFORM
    PLATFORM_FONT_IMMEDIATE=1
    PLATFORM_SIGN_TEXT_CACHE=1
    PLATFORM_PRECACHE_GUI_TEXTURES=1
    PC_LEGACY_BUILD=1
    PLATFORM_SOFTWARE_CURSOR=1
    PLATFORM_LOCAL_RESOURCES_ONLY=1
    PLATFORM_OPTIFINE_CUSTOM_ANIMATIONS=0
    PLATFORM_OPTIFINE_RANDOM_MOBS=0
    PLATFORM_OPTIFINE_CUSTOM_FONTS=0
    PLATFORM_FAST_REGION_COMPRESSION=1
    MC_LOG_LEVEL=${MC_LOG_LEVEL}
    ${VITA_PROFILE_DEFINITIONS}
)

target_include_directories(OptiCraft PRIVATE
    "${CMAKE_SOURCE_DIR}/vita/include"
    "${VITA_DEPS}/include/SDL2"
    "${VITA_DEPS}/include"
    "${VITASDK}/arm-vita-eabi/include/SDL2"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/pc"
    "${CMAKE_SOURCE_DIR}/src/net/minecraft/src"
    "${CMAKE_SOURCE_DIR}/src/mods"
    "${CMAKE_SOURCE_DIR}/external/stb"
    "${CMAKE_SOURCE_DIR}/external/miniaudio"
    "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip"
)

# --undefined keeps the segment pad in src/pc/Main.cpp through --gc-sections.
target_link_options(OptiCraft PRIVATE -Wl,-q -Wl,--gc-sections -Wl,--undefined=g_vitaSegmentPad)

target_link_libraries(OptiCraft PRIVATE
    SDL2_net
    "${VITA_DEPS}/lib/libSDL2.a"
    "${VITA_DEPS}/lib/libvitaGL.a"
    vitashark
    SceShaccCgExt
    SceShaccCg_stub
    mathneon
    taihen_stub
    z
    -Wl,--whole-archive
    pthread
    -Wl,--no-whole-archive
    SceGxm_stub
    SceDisplay_stub
    SceCtrl_stub
    SceAppMgr_stub
    SceAppUtil_stub
    SceAudio_stub
    SceAudioIn_stub
    SceSysmodule_stub
    SceIofilemgr_stub
    SceCommonDialog_stub
    SceTouch_stub
    SceHid_stub
    SceMotion_stub
    ScePower_stub
    SceProcessmgr_stub
    SceKernelDmacMgr_stub
    SceIme_stub
    SceNet_stub
    SceNetCtl_stub
    SceLibKernel_stub
    m
)

vita_create_self(eboot.bin OptiCraft UNSAFE)
vita_create_vpk(${VITA_APP_NAME}.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/vita/sce_sys/icon0.png sce_sys/icon0.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/vita/sce_sys/pic0.png sce_sys/pic0.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/vita/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/vita/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/vita/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
