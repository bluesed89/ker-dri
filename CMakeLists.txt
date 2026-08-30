cmake_minimum_required(VERSION 3.20)
project(AlbionDumper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Ortak derleme ayarları ──────────────────────────────────────────
if(MSVC)
    add_compile_options(/W3 /permissive- /EHa)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra)
endif()

# ── Standalone Circular Overlay Radar ───────────────────────────────
add_executable(standalone_radar WIN32 src/standalone_radar.cpp)
target_link_libraries(standalone_radar PRIVATE user32 gdi32)

# ── Radar Target ────────────────────────────────────────────────────
set(IMGUI_SOURCES
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/imgui_impl_win32.cpp
    external/imgui/imgui_impl_dx11.cpp
)

add_executable(radar src/radar_imgui.cpp ${IMGUI_SOURCES})
target_include_directories(radar PRIVATE external/imgui)
target_link_libraries(radar PRIVATE user32 gdi32 advapi32 d3d11 d3dcompiler dwmapi)

# ── Proxy DLL (version.dll — Oyunun klasörüne konulacak) ────────────
add_library(version SHARED src/proxy.cpp)
target_include_directories(version PRIVATE include)
target_link_libraries(version PRIVATE user32)
set_target_properties(version PROPERTIES 
    PREFIX "" 
    OUTPUT_NAME "version"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/proxy"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/proxy"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/proxy"
)

if(MSVC)
    set_property(TARGET version PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ── Dumper DLL & Injector ───────────────────────────────────────────
add_library(dumper SHARED src/dumper.cpp)
target_include_directories(dumper PRIVATE include)
target_link_libraries(dumper PRIVATE user32)

add_executable(injector src/injector.cpp)
target_link_libraries(injector PRIVATE advapi32)

add_executable(mapper src/mapper.cpp)
target_link_libraries(mapper PRIVATE advapi32)

# ── Standalone Kernel Driver Radar Target ───────────────────────────
add_executable(standalone_radar_drv_v2 WIN32 kernel_radar/user/standalone_radar_drv.cpp)
target_include_directories(standalone_radar_drv_v2 PRIVATE kernel_radar/shared kernel_radar/user)
target_link_libraries(standalone_radar_drv_v2 PRIVATE user32 gdi32 advapi32)

# ── Çıktıları bin klasörüne topla ───────────────────────────────────
set_target_properties(standalone_radar radar standalone_radar_drv_v2 dumper injector mapper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    string(TOUPPER ${config} config_upper)
    set_target_properties(standalone_radar radar standalone_radar_drv_v2 dumper injector mapper PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin"
    )
    set_target_properties(version PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin/proxy"
        LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin/proxy"
        ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin/proxy"
    )
endforeach()
