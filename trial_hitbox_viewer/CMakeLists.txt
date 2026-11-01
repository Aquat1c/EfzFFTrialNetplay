cmake_minimum_required(VERSION 3.20)

project(EFZNetplay LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "EFZ trial is 32-bit. Reconfigure with: cmake -A Win32 ...")
endif()

# MinHook is vendored under this project's lib/ tree.
set(EFZ_MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lib/minhook_src"
    CACHE PATH "MinHook source tree")
if(NOT EXISTS "${EFZ_MINHOOK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "MinHook not found at ${EFZ_MINHOOK_DIR}")
endif()

set(THV_SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lib/SDL3" CACHE PATH "SDL3 prebuilt tree")
set(THV_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lib/imgui" CACHE PATH "Dear ImGui source tree")

set(THV_SDL3_ARCH "x86")
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(THV_SDL3_ARCH "x64")
endif()
set(THV_SDL3_LIB "${THV_SDL3_DIR}/lib/${THV_SDL3_ARCH}/SDL3.lib")
set(THV_SDL3_DLL "${THV_SDL3_DIR}/lib/${THV_SDL3_ARCH}/SDL3.dll")
if(NOT EXISTS "${THV_SDL3_LIB}" OR NOT EXISTS "${THV_SDL3_DLL}")
    message(FATAL_ERROR "SDL3 not found at ${THV_SDL3_DIR}/lib/${THV_SDL3_ARCH}")
endif()
if(NOT EXISTS "${THV_IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR "Dear ImGui not found at ${THV_IMGUI_DIR}")
endif()

option(THV_STATIC_RUNTIME "Link MSVC runtime statically" ON)
if(MSVC AND THV_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${EFZ_MINHOOK_DIR}" "${CMAKE_BINARY_DIR}/minhook" EXCLUDE_FROM_ALL)

if(MSVC AND THV_STATIC_RUNTIME AND TARGET minhook)
    set_property(TARGET minhook PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(THV_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(TRIAL_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shared")

add_library(netplay SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/netplay.rc
    ${THV_SRC}/core/dllmain.cpp
    ${TRIAL_SHARED_DIR}/autopunch_client.cpp

    ${THV_SRC}/hooks/collision_display.cpp
    ${THV_SRC}/hooks/control_swap.cpp
    ${THV_SRC}/hooks/dinput_focus_guard.cpp
    ${THV_SRC}/hooks/dinput_relay.cpp
    ${THV_SRC}/hooks/framestep.cpp
    ${THV_SRC}/hooks/game_pacing.cpp
    ${THV_SRC}/hooks/render_gate.cpp
    ${THV_SRC}/hooks/trial_gamespeed.cpp

    ${THV_SRC}/util/determinism.cpp
    ${THV_SRC}/util/rng.cpp
    ${THV_SRC}/util/debug_log.cpp
    ${THV_SRC}/util/crash_handler.cpp
    ${THV_SRC}/util/lifecycle_log.cpp
    ${THV_SRC}/util/mem_write_log.cpp
    ${THV_SRC}/util/test_harness.cpp
    ${THV_SRC}/util/watchdog.cpp
    ${THV_SRC}/util/win32_compat.cpp
    ${THV_SRC}/util/state_paths.cpp

    ${THV_SRC}/practice/savestate.cpp
    ${THV_SRC}/practice/savestate_digest.cpp
    ${THV_SRC}/practice/savestate_layout.cpp
    ${THV_SRC}/practice/savestate_profiler.cpp

    ${THV_SRC}/caster/caster_telemetry.cpp
    ${THV_SRC}/caster/caster_ui.cpp
    ${THV_SRC}/caster/caster_session.cpp
    ${THV_SRC}/caster/window_telemetry.cpp
      ${THV_SRC}/caster/spectator_checkpoint.cpp
      ${THV_SRC}/caster/spectator_relay.cpp
      ${THV_SRC}/caster/spectator_client.cpp
      ${THV_SRC}/caster/spectator_player.cpp
    ${THV_SRC}/caster/control_config.cpp
    ${THV_SRC}/caster/netplay_bootstrap.cpp
    ${THV_SRC}/caster/netplay_navigation.cpp
    ${THV_SRC}/caster/netplay_delay.cpp
    ${THV_SRC}/caster/netplay_lockstep.cpp
    ${THV_SRC}/caster/netplay_sync.cpp
    ${THV_SRC}/caster/netplay_frame_gate.cpp
    ${THV_SRC}/caster/menu_nav.cpp
    ${THV_SRC}/caster/caster_input.cpp
    ${THV_SRC}/caster/netplay_input.cpp
    ${THV_SRC}/caster/trial_input_codec.cpp
    ${THV_SRC}/caster/sdl_input.cpp

    ${THV_SRC}/rollback/rollback.cpp
    ${THV_SRC}/rollback/rollback_session.cpp
    ${THV_SRC}/rollback/rollback_engine.cpp
    ${THV_SRC}/rollback/rollback_memory.cpp
    ${THV_SRC}/rollback/desync_check.cpp

    "${THV_IMGUI_DIR}/imgui.cpp"
    "${THV_IMGUI_DIR}/imgui_draw.cpp"
    "${THV_IMGUI_DIR}/imgui_tables.cpp"
    "${THV_IMGUI_DIR}/imgui_widgets.cpp"
    "${THV_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${THV_IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp"
)

target_include_directories(netplay PRIVATE
    "${EFZ_MINHOOK_DIR}/include"
    "${TRIAL_SHARED_DIR}"
    "${THV_SDL3_DIR}/include"
    "${THV_IMGUI_DIR}"
    "${THV_IMGUI_DIR}/backends"
    "${THV_SRC}/core"
    "${THV_SRC}/util"
    "${THV_SRC}/hooks"
    "${THV_SRC}/practice"
    "${THV_SRC}/caster"
    "${THV_SRC}/rollback"
)

target_compile_definitions(netplay PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    SDL_MAIN_HANDLED
    IMGUI_DISABLE_DEMO_WINDOWS
)

target_link_libraries(netplay PRIVATE minhook ws2_32 dinput8 ddraw winmm "${THV_SDL3_LIB}")

set(THV_DIST_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dist/game" CACHE PATH "Deploy folder for game-ready binaries")

if(MSVC)
    target_compile_options(netplay PRIVATE /W4 /EHsc)
    # Emit a PDB even in Release so crashes symbolize.
    target_compile_options(netplay PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(netplay PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
    set_target_properties(netplay PROPERTIES
        OUTPUT_NAME "netplay"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_custom_command(TARGET netplay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${THV_SDL3_DLL}"
            "$<TARGET_FILE_DIR:netplay>"
        COMMENT "Copy SDL3.dll beside netplay.dll"
    )

    add_custom_target(deploy_mod
        COMMAND ${CMAKE_COMMAND} -E make_directory "${THV_DIST_GAME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:netplay>"
            "${THV_DIST_GAME_DIR}/netplay.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${THV_SDL3_DLL}"
            "${THV_DIST_GAME_DIR}/SDL3.dll"
        DEPENDS netplay
        COMMENT "Copy netplay.dll + SDL3.dll to dist/game"
    )
endif()

add_executable(spectator_checkpoint_tests
    tests/spectator_checkpoint_tests.cpp
    ${THV_SRC}/caster/spectator_checkpoint.cpp
    ${THV_SRC}/caster/spectator_relay.cpp
)

add_executable(control_config_tests
    tests/control_config_tests.cpp
    ${THV_SRC}/caster/control_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../trial_caster/src/control_bindings.cpp
)

add_executable(win32_compat_tests
    tests/win32_compat_tests.cpp
    ${THV_SRC}/util/win32_compat.cpp
    ${THV_SRC}/util/state_paths.cpp
)
add_executable(netplay_delay_tests
    tests/netplay_delay_tests.cpp
    ${THV_SRC}/caster/netplay_delay.cpp
)
add_executable(debug_log_tests
    tests/debug_log_tests.cpp
    ${THV_SRC}/util/debug_log.cpp
    ${THV_SRC}/util/state_paths.cpp
)
target_include_directories(netplay_delay_tests PRIVATE
    "${TRIAL_SHARED_DIR}"
    "${THV_SRC}/caster"
    "${THV_SRC}/core"
)
target_include_directories(debug_log_tests PRIVATE
    "${THV_SRC}/util"
)
target_compile_definitions(debug_log_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)
if(MSVC)
    target_compile_options(debug_log_tests PRIVATE /W4 /permissive-)
endif()
set_target_properties(debug_log_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
if(MSVC)
    target_compile_options(netplay_delay_tests PRIVATE /W4 /permissive-)
endif()
set_target_properties(netplay_delay_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
target_include_directories(win32_compat_tests PRIVATE
    "${THV_SRC}/util"
)
target_compile_definitions(win32_compat_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
if(MSVC)
    target_compile_options(win32_compat_tests PRIVATE /W4 /permissive-)
endif()
set_target_properties(win32_compat_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
target_include_directories(control_config_tests PRIVATE
    "${TRIAL_SHARED_DIR}"
    "${THV_SDL3_DIR}/include"
    "${THV_SRC}/util"
    "${THV_SRC}/caster"
    "${CMAKE_CURRENT_SOURCE_DIR}/../trial_caster/src"
)
target_compile_definitions(control_config_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(control_config_tests PRIVATE "${THV_SDL3_LIB}")
if(MSVC)
    target_compile_options(control_config_tests PRIVATE /W4 /permissive-)
endif()
set_target_properties(control_config_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
add_custom_command(TARGET control_config_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${THV_SDL3_DLL}"
        "$<TARGET_FILE_DIR:control_config_tests>/SDL3.dll"
)
target_include_directories(spectator_checkpoint_tests PRIVATE
    "${TRIAL_SHARED_DIR}"
    "${THV_SRC}/core"
    "${THV_SRC}/util"
    "${THV_SRC}/caster"
)
target_compile_definitions(spectator_checkpoint_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
if(MSVC)
    target_compile_options(spectator_checkpoint_tests PRIVATE /W4 /permissive-)
endif()
set_target_properties(spectator_checkpoint_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
if(BUILD_TESTING)
    add_test(NAME spectator_checkpoint_codec COMMAND spectator_checkpoint_tests)
    add_test(NAME control_config_aliases COMMAND control_config_tests)
    add_test(NAME win32_wine_compat COMMAND win32_compat_tests)
    add_test(NAME netplay_live_delay COMMAND netplay_delay_tests)
    add_test(NAME debug_log_policy COMMAND debug_log_tests)
endif()
