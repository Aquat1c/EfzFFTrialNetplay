cmake_minimum_required(VERSION 3.20)

project(TrialLauncher LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "EFZ trial is 32-bit. Reconfigure with: cmake -A Win32 ...")
endif()

set(TC_SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lib/SDL3" CACHE PATH "SDL3 prebuilt tree")
set(TC_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lib/imgui" CACHE PATH "Dear ImGui source tree")

set(TC_SDL3_ARCH "x86")
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(TC_SDL3_ARCH "x64")
endif()
set(TC_SDL3_LIB "${TC_SDL3_DIR}/lib/${TC_SDL3_ARCH}/SDL3.lib")
set(TC_SDL3_DLL "${TC_SDL3_DIR}/lib/${TC_SDL3_ARCH}/SDL3.dll")
if(NOT EXISTS "${TC_SDL3_LIB}" OR NOT EXISTS "${TC_SDL3_DLL}")
    message(FATAL_ERROR "SDL3 not found at ${TC_SDL3_DIR}/lib/${TC_SDL3_ARCH}")
endif()
if(NOT EXISTS "${TC_IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR "Dear ImGui not found at ${TC_IMGUI_DIR}")
endif()

option(TC_STATIC_RUNTIME "Link MSVC runtime statically" ON)
if(MSVC AND TC_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

set(TC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(THV_UTIL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../trial_hitbox_viewer/src/util")
set(TRIAL_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shared")

set(TC_IMGUI_SOURCES
    ${TC_IMGUI_DIR}/imgui.cpp
    ${TC_IMGUI_DIR}/imgui_draw.cpp
    ${TC_IMGUI_DIR}/imgui_tables.cpp
    ${TC_IMGUI_DIR}/imgui_widgets.cpp
    ${TC_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${TC_IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)

add_executable(TrialLauncher WIN32
    ${TC_SRC}/main.cpp
    ${TC_SRC}/caster_app.cpp
    ${TC_SRC}/compatibility_runtime.cpp
    ${TC_SRC}/caster_config.cpp
    ${TC_SRC}/control_bindings.cpp
    ${TC_SRC}/caster_ui.cpp
    ${TC_SRC}/ip_info.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/launcher.rc
    ${TC_SRC}/game_launcher.cpp
    ${TC_SRC}/net_session.cpp
    ${TC_SRC}/protocol.cpp
    ${TC_SRC}/session_file.cpp
    ${TC_SRC}/session_log.cpp
    ${TC_SRC}/util.cpp
    ${TC_SRC}/version_info.cpp
    ${TC_SRC}/launcher_log_stubs.cpp
    ${TRIAL_SHARED_DIR}/autopunch_client.cpp
    ${THV_UTIL_DIR}/crash_handler.cpp
    ${THV_UTIL_DIR}/debug_log.cpp
    ${THV_UTIL_DIR}/state_paths.cpp
    ${THV_UTIL_DIR}/win32_compat.cpp
    ${TC_IMGUI_SOURCES}
)

target_include_directories(TrialLauncher PRIVATE
    ${TC_SRC}
    ${TRIAL_SHARED_DIR}
    ${THV_UTIL_DIR}
    ${TC_SDL3_DIR}/include
    ${TC_IMGUI_DIR}
    ${TC_IMGUI_DIR}/backends
)

target_compile_definitions(TrialLauncher PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(TrialLauncher PRIVATE
    ${TC_SDL3_LIB}
    ole32
    windowscodecs
    ws2_32
    winmm
    wininet
    version
)

if(MSVC)
    target_compile_options(TrialLauncher PRIVATE /W4 /permissive-)
endif()

set(TC_OUT_DIR "${CMAKE_BINARY_DIR}/bin")
set_target_properties(TrialLauncher PROPERTIES
    OUTPUT_NAME "EFZ FF Netplay"
    RUNTIME_OUTPUT_DIRECTORY "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TC_OUT_DIR}"
)

set(TC_DIST_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dist/game" CACHE PATH "Deploy folder for game-ready binaries")

add_custom_command(TARGET TrialLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${TC_SDL3_DLL}"
        "$<TARGET_FILE_DIR:TrialLauncher>/SDL3.dll"
    COMMENT "Copy SDL3.dll beside EFZ FF Netplay.exe"
)

add_custom_target(deploy_launcher
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TC_DIST_GAME_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:TrialLauncher>"
        "${TC_DIST_GAME_DIR}/EFZ FF Netplay.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${TC_SDL3_DLL}"
        "${TC_DIST_GAME_DIR}/SDL3.dll"
    DEPENDS TrialLauncher
    COMMENT "Copy EFZ FF Netplay.exe + SDL3.dll to dist/game"
)

add_executable(hash_efz tools/hash_efz.cpp)
target_include_directories(hash_efz PRIVATE ${TC_SRC})
target_link_libraries(hash_efz PRIVATE)
set_target_properties(hash_efz PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TC_OUT_DIR}"
)

add_executable(launch_smoke tools/launch_smoke.cpp ${TC_SRC}/game_launcher.cpp ${TC_SRC}/compatibility_runtime.cpp ${TC_SRC}/session_log.cpp ${TC_SRC}/util.cpp)
target_include_directories(launch_smoke PRIVATE ${TC_SRC})
target_compile_definitions(launch_smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(launch_smoke PRIVATE ws2_32)
if(MSVC)
    target_compile_options(launch_smoke PRIVATE /W4 /permissive-)
endif()
set_target_properties(launch_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TC_OUT_DIR}"
)

add_executable(trial_protocol_tests
    tests/protocol_tests.cpp
)
target_include_directories(trial_protocol_tests PRIVATE ${TRIAL_SHARED_DIR})
if(MSVC)
    target_compile_options(trial_protocol_tests PRIVATE /W4 /permissive-)
endif()
set_target_properties(trial_protocol_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TC_OUT_DIR}"
)

add_executable(trial_autopunch_client_tests
    tests/autopunch_client_tests.cpp
    ${TRIAL_SHARED_DIR}/autopunch_client.cpp
)
target_include_directories(trial_autopunch_client_tests PRIVATE ${TRIAL_SHARED_DIR})
target_compile_definitions(trial_autopunch_client_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(trial_autopunch_client_tests PRIVATE ws2_32)
if(MSVC)
    target_compile_options(trial_autopunch_client_tests PRIVATE /W4 /permissive-)
endif()
set_target_properties(trial_autopunch_client_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TC_OUT_DIR}"
)

add_executable(trial_net_session_tests
    tests/net_session_tests.cpp
    ${TC_SRC}/caster_config.cpp
    ${TC_SRC}/compatibility_runtime.cpp
    ${TC_SRC}/control_bindings.cpp
    ${TC_SRC}/net_session.cpp
    ${TC_SRC}/protocol.cpp
    ${TC_SRC}/session_file.cpp
    ${TC_SRC}/session_log.cpp
    ${TC_SRC}/util.cpp
    ${TRIAL_SHARED_DIR}/autopunch_client.cpp
)

add_executable(trial_control_bindings_tests
    tests/control_bindings_tests.cpp
    ${TC_SRC}/control_bindings.cpp
)
target_include_directories(trial_control_bindings_tests PRIVATE
    ${TC_SRC}
    ${TRIAL_SHARED_DIR}
    ${TC_SDL3_DIR}/include
)
target_compile_definitions(trial_control_bindings_tests PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(trial_control_bindings_tests PRIVATE ${TC_SDL3_LIB})
if(MSVC)
    target_compile_options(trial_control_bindings_tests PRIVATE /W4 /permissive-)
endif()
set_target_properties(trial_control_bindings_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TC_OUT_DIR}"
)
add_custom_command(TARGET trial_control_bindings_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${TC_SDL3_DLL}"
        "$<TARGET_FILE_DIR:trial_control_bindings_tests>/SDL3.dll"
)
target_include_directories(trial_net_session_tests PRIVATE
    ${TC_SRC}
    ${TRIAL_SHARED_DIR}
    ${TC_SDL3_DIR}/include
)
target_compile_definitions(trial_net_session_tests PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(trial_net_session_tests PRIVATE ws2_32 winmm ${TC_SDL3_LIB})
if(MSVC)
    target_compile_options(trial_net_session_tests PRIVATE /W4 /permissive-)
endif()
set_target_properties(trial_net_session_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TC_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TC_OUT_DIR}"
)
add_custom_command(TARGET trial_net_session_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${TC_SDL3_DLL}"
        "$<TARGET_FILE_DIR:trial_net_session_tests>/SDL3.dll"
)

if(BUILD_TESTING)
    set(TRIAL_NETPLAY_DLL_FOR_TEST
        "${CMAKE_CURRENT_SOURCE_DIR}/../trial_hitbox_viewer/build/bin/Release/netplay.dll"
        CACHE FILEPATH "Release netplay DLL used by the launcher compatibility-contract test")
    set(TRIAL_EFZ_EXE_FOR_TEST
        ""
        CACHE FILEPATH "Supported EFZ executable used by the launcher PE-layout test")
    set(TRIAL_DDRAW_DLL_FOR_TEST
        "${CMAKE_CURRENT_SOURCE_DIR}/../ddraw_wrapper/build/Release/ddraw.dll"
        CACHE FILEPATH "Packaged EFZ DirectDraw wrapper used by the export-contract test")
    add_test(
        NAME trial_protocol_codec
        COMMAND trial_protocol_tests "${TRIAL_NETPLAY_DLL_FOR_TEST}")
    set_tests_properties(trial_protocol_codec PROPERTIES
        REQUIRED_FILES "${TRIAL_NETPLAY_DLL_FOR_TEST}")
    add_test(
        NAME trial_launcher_session
        COMMAND trial_net_session_tests
            "${TRIAL_NETPLAY_DLL_FOR_TEST}"
            "${TRIAL_EFZ_EXE_FOR_TEST}"
            "${TRIAL_DDRAW_DLL_FOR_TEST}")
    set_tests_properties(trial_launcher_session PROPERTIES
        REQUIRED_FILES "${TRIAL_NETPLAY_DLL_FOR_TEST};${TRIAL_DDRAW_DLL_FOR_TEST}")
    add_test(NAME trial_control_bindings COMMAND trial_control_bindings_tests)
    add_test(NAME trial_autopunch_client COMMAND trial_autopunch_client_tests)

    add_executable(trial_version_info_tests
        tests/version_info_tests.cpp
        ${TC_SRC}/version_info.cpp
    )
    target_include_directories(trial_version_info_tests PRIVATE
        ${TC_SRC}
        ${TRIAL_SHARED_DIR}
    )
    target_compile_definitions(trial_version_info_tests PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_link_libraries(trial_version_info_tests PRIVATE version)
    if(MSVC)
        target_compile_options(trial_version_info_tests PRIVATE /W4 /permissive-)
    endif()
    set_target_properties(trial_version_info_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TC_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TC_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TC_OUT_DIR}"
    )
    add_dependencies(trial_version_info_tests TrialLauncher)
    add_test(
        NAME trial_component_versions
        COMMAND trial_version_info_tests
            "$<TARGET_FILE:TrialLauncher>"
            "${TRIAL_NETPLAY_DLL_FOR_TEST}")
    set_tests_properties(trial_component_versions PROPERTIES
        REQUIRED_FILES "$<TARGET_FILE:TrialLauncher>;${TRIAL_NETPLAY_DLL_FOR_TEST}")

    add_executable(trial_launch_target WIN32 tests/launch_target.cpp)
    set_target_properties(trial_launch_target PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TC_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TC_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TC_OUT_DIR}"
    )

    add_library(trial_launch_probe SHARED tests/launch_probe.cpp)
    set_target_properties(trial_launch_probe PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${TC_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TC_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TC_OUT_DIR}"
    )

    add_executable(trial_game_launcher_tests
        tests/game_launcher_tests.cpp
        ${TC_SRC}/game_launcher.cpp
        ${TC_SRC}/compatibility_runtime.cpp
        ${TC_SRC}/session_log.cpp
        ${TC_SRC}/util.cpp
    )
    target_include_directories(trial_game_launcher_tests PRIVATE ${TC_SRC})
    target_compile_definitions(trial_game_launcher_tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(trial_game_launcher_tests PRIVATE ws2_32)
    if(MSVC)
        target_compile_options(trial_game_launcher_tests PRIVATE /W4 /permissive-)
    endif()
    set_target_properties(trial_game_launcher_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TC_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TC_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TC_OUT_DIR}"
    )
    add_test(
        NAME trial_game_injection
        COMMAND trial_game_launcher_tests
            "$<TARGET_FILE:trial_launch_target>"
            "$<TARGET_FILE:trial_launch_probe>")
    set_tests_properties(trial_game_injection PROPERTIES TIMEOUT 45)
endif()
