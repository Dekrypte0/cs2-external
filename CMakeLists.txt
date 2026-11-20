cmake_minimum_required(VERSION 3.15)
project(cs2_helper)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# static crt — single self-contained exe, no redistributable dll trail
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

function(add_cs2_helper_target target_name)
    add_executable(${target_name}
        main.cpp
        overlay.cpp
        renderer.cpp
        ${IMGUI_SOURCES}
    )
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
    )
    target_link_libraries(${target_name} PRIVATE
        d3d11
        dwmapi
        user32
        gdi32
    )
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /wd4201 /wd4244 /wd4267)
        # background app: no console window is created at all. diagnostics go
        # to helper_debug.log, and the INSERT/F2/END keybinds still work.
        target_link_options(${target_name} PRIVATE /DEBUG:NONE /INCREMENTAL:NO
            /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    endif()
endfunction()

add_cs2_helper_target(cs2_helper_new)
add_cs2_helper_target(cs2_helper_fast)
