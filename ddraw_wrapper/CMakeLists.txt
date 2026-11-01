cmake_minimum_required(VERSION 3.15)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(ddraw_wrapper LANGUAGES C CXX)

option(
    DDRAW_WRAPPER_ENABLE_LOGGING
    "Compile opt-in DirectDraw wrapper diagnostic logging support"
    OFF
)

set(EFZ_IMGUI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lib/imgui")
if(NOT EXISTS "${EFZ_IMGUI_SOURCE_DIR}/imgui.h")
    message(FATAL_ERROR "Dear ImGui reference sources were not found at ${EFZ_IMGUI_SOURCE_DIR}")
endif()

add_library(ddraw SHARED
    ddraw.c
    render_dx9_imgui.cpp
    ddraw.def          # provides the __declspec-less ordinal export table
    "${EFZ_IMGUI_SOURCE_DIR}/imgui.cpp"
    "${EFZ_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
    "${EFZ_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
    "${EFZ_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
    "${EFZ_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
    "${EFZ_IMGUI_SOURCE_DIR}/backends/imgui_impl_dx9.cpp"
    "${EFZ_IMGUI_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
)

target_include_directories(ddraw PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${EFZ_IMGUI_SOURCE_DIR}"
    "${EFZ_IMGUI_SOURCE_DIR}/backends"
)

target_compile_features(ddraw PRIVATE cxx_std_17)

target_compile_definitions(ddraw PRIVATE
    UNICODE
    _UNICODE
)

if(DDRAW_WRAPPER_ENABLE_LOGGING)
    target_compile_definitions(ddraw PRIVATE DDRAW_WRAPPER_ENABLE_LOGGING=1)
else()
    target_compile_definitions(ddraw PRIVATE DDRAW_WRAPPER_ENABLE_LOGGING=0)
endif()

if(MSVC)
    set_property(TARGET ddraw PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(ddraw PRIVATE
        /W3           # warning level
        /GS-          # no security cookies (unnecessary overhead for a thin shim)
        /wd4100       # suppress unreferenced formal parameter
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Debug>:/Zi>
        $<$<CONFIG:Release>:/O2>
    )
    target_link_libraries(ddraw PRIVATE
        kernel32
        user32
        gdi32
        d3d9
        dwmapi
        imm32
    )
    target_link_options(ddraw PRIVATE
        /MACHINE:X86
        /DYNAMICBASE
        /NXCOMPAT
        /SUBSYSTEM:WINDOWS,6.0
        $<$<CONFIG:Debug>:/DEBUG>
    )
else()
    target_compile_options(ddraw PRIVATE
        -Wall
        -Wno-unused-parameter
        -m32                       # must match the 32-bit game process
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Release>:-O2>
    )
    target_link_options(ddraw PRIVATE
        -m32
        -static
        -static-libgcc             # bundle libgcc so the game dir needs no runtime
        -Wl,--kill-at              # strip stdcall @N suffix from exports
        -Wl,--enable-stdcall-fixup
    )
    target_link_libraries(ddraw PRIVATE
        kernel32
        user32
        gdi32
        d3d9
        dwmapi
        imm32
    )
endif()

set_target_properties(ddraw PROPERTIES
    OUTPUT_NAME   "ddraw"
    PREFIX        ""
    SUFFIX        ".dll"
)

install(TARGETS ddraw RUNTIME DESTINATION .)
