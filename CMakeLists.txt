cmake_minimum_required(VERSION 3.16)

project(WhiteholePro VERSION 0.2.0 LANGUAGES CXX)

option(WHITEHOLE_BUILD_TESTS "Build the native core tests" ON)
option(WHITEHOLE_BUILD_GUI "Build the windowed whitehole-pro application (Windows)" ON)
option(WHITEHOLE_STATIC_RUNTIME "Link the C/C++ runtime statically for a portable single-file executable" ON)
option(WHITEHOLE_ENABLE_LTO "Enable link-time optimisation for Release builds" OFF)

# Single-config generators (Makefiles, Ninja) need an explicit build type.
# Defaulting to Release means a plain `cmake --build build` already produces an
# optimised executable instead of an unoptimised one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    # Only set the cache-variable property when CMAKE_BUILD_TYPE is actually a
    # cache entry.  With multi-config generators (Visual Studio, Xcode) the
    # variable is not created, so set_property(CACHE ...) would error out.
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WHITEHOLE_STATIC_RUNTIME)
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        add_link_options(-static -static-libgcc -static-libstdc++)
    endif()
endif()

# Release profile shared by every target.
function(whitehole_optimize target)
    if(MSVC)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O2 /Gy>)
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
    else()
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>)
        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:-Wl,--gc-sections>
            $<$<CONFIG:Release>:-s>)
        if(WHITEHOLE_ENABLE_LTO)
            target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-flto=auto>)
            target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-flto=auto>)
        endif()
    endif()
endfunction()

add_library(whitehole_core
    cpp/src/app/settings.cpp
    cpp/src/db/name_table.cpp
    cpp/src/db/object_db.cpp
        cpp/src/db/data_holder.cpp
    cpp/src/db/custom_obj_db.cpp
    cpp/src/db/hints.cpp
    cpp/src/db/areamanagerlimits.cpp
    cpp/src/db/shortcuts.cpp
    cpp/src/db/modelsubstitutions.cpp
    cpp/src/db/specialrenderers.cpp
    cpp/src/app/theme_palette.cpp
    cpp/src/edit/authoring.cpp
    cpp/src/edit/commands.cpp
    cpp/src/edit/rails.cpp
    cpp/src/edit/document.cpp
    cpp/src/edit/undo.cpp
    cpp/src/edit/validation.cpp
    cpp/src/io/binary_file.cpp
    cpp/src/io/directory_filesystem.cpp
    cpp/src/io/rarc.cpp
    cpp/src/io/yaz0.cpp
    cpp/src/render/camera.cpp
    cpp/src/render/camera_controller.cpp
    cpp/src/render/camera_tween.cpp
    cpp/src/render/collision_kcl.cpp
    cpp/src/render/gizmo.cpp
    cpp/src/render/object_visual.cpp
    cpp/src/render/model_mesh.cpp
    cpp/src/render/model_library.cpp
    cpp/src/render/surface_snap.cpp
    cpp/src/render/viewport_scene.cpp
    cpp/src/smg/bcsv.cpp
    cpp/src/smg/field_hashes.cpp
    cpp/src/smg/galaxy_archive.cpp
    cpp/src/smg/game_archive.cpp
    cpp/src/smg/hash.cpp
    cpp/src/smg/bti.cpp
    cpp/src/smg/bmd.cpp
    cpp/src/smg/object_model.cpp
    cpp/src/smg/path.cpp
    cpp/src/smg/stage_archive.cpp
    cpp/src/util/json.cpp
)

target_include_directories(whitehole_core PUBLIC cpp/include)
target_compile_features(whitehole_core PUBLIC cxx_std_20)
set_target_properties(whitehole_core PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(whitehole_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(whitehole_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()
whitehole_optimize(whitehole_core)

# ---------------------------------------------------------------------------
# Windows detection and the shared Win32 import set
# ---------------------------------------------------------------------------
set(WHITEHOLE_HAVE_WIN32_GUI OFF)
if(WIN32 OR MINGW)
    set(WHITEHOLE_HAVE_WIN32_GUI ON)
elseif(DEFINED ENV{MSYSTEM} AND NOT "$ENV{MSYSTEM}" STREQUAL "MSYS")
    set(WHITEHOLE_HAVE_WIN32_GUI ON)
elseif(CMAKE_CXX_COMPILER MATCHES "ucrt64|mingw64|mingw32")
    set(WHITEHOLE_HAVE_WIN32_GUI ON)
endif()

add_library(whitehole_win32 INTERFACE)
if(WHITEHOLE_HAVE_WIN32_GUI)
    target_compile_definitions(whitehole_win32 INTERFACE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(whitehole_win32 INTERFACE comctl32 ole32 uuid shell32 comdlg32 opengl32 gdi32 winhttp d3d11 dxgi dwmapi d3dcompiler)
endif()

# ---------------------------------------------------------------------------
# Dear ImGui (docking branch), vendored so the build stays offline and every
# executable remains a single file with no extra DLLs.
# ---------------------------------------------------------------------------
if(WHITEHOLE_HAVE_WIN32_GUI)
    add_library(whitehole_imgui STATIC
        cpp/third_party/imgui/imgui.cpp
        cpp/third_party/imgui/imgui_draw.cpp
        cpp/third_party/imgui/imgui_tables.cpp
        cpp/third_party/imgui/imgui_widgets.cpp
        cpp/third_party/imgui/backends/imgui_impl_win32.cpp
        cpp/third_party/imgui/backends/imgui_impl_dx11.cpp
    )
    target_include_directories(whitehole_imgui SYSTEM PUBLIC cpp/third_party/imgui)
    target_compile_features(whitehole_imgui PUBLIC cxx_std_20)
    set_target_properties(whitehole_imgui PROPERTIES CXX_EXTENSIONS OFF)
    # Keep third-party diagnostics quiet; our own code keeps full warnings.
    if(MSVC)
        target_compile_options(whitehole_imgui PRIVATE /W1 /permissive- /utf-8)
    else()
        target_compile_options(whitehole_imgui PRIVATE -w)
    endif()
    if(WIN32)
        target_link_libraries(whitehole_imgui PUBLIC d3d11 d3dcompiler dwmapi)
    endif()
    whitehole_optimize(whitehole_imgui)
endif()

# ---------------------------------------------------------------------------
# Application objects shared by both executables (compiled only once)
# ---------------------------------------------------------------------------
set(WHITEHOLE_GUI_SOURCE cpp/src/app/gui_win32.cpp cpp/src/app/gui_theme.cpp cpp/src/render/viewport_win32.cpp)
if(NOT WHITEHOLE_HAVE_WIN32_GUI)
    set(WHITEHOLE_GUI_SOURCE cpp/src/app/gui_stub.cpp)
endif()

add_library(whitehole_app OBJECT
    cpp/src/app/cli.cpp
    cpp/src/app/object_db_update.cpp
    ${WHITEHOLE_GUI_SOURCE}
)
target_link_libraries(whitehole_app PRIVATE whitehole_core whitehole_win32)
if(WHITEHOLE_HAVE_WIN32_GUI)
    # The ImGui-backed GUI lives in the shared app objects, so both executables
    # need the vendored Dear ImGui static library on the link line.
    target_link_libraries(whitehole_app PRIVATE whitehole_imgui)
endif()
target_compile_definitions(whitehole_app PRIVATE
    WHITEHOLE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
# The GUI sources carry UTF-8 text (em dashes, ellipses in panel copy), so MSVC
# must be told the source charset explicitly -- without it the compiler falls
# back to the system code page and both narrow and wide literals are mangled.
if(MSVC)
    target_compile_options(whitehole_app PRIVATE /utf-8 /permissive-)
else()
    # Same warning set whitehole_core uses, so the editor sources are held to
    # the standard the rest of the tree is. Vendored ImGui arrives through a
    # SYSTEM include (see whitehole_imgui), so it contributes no diagnostics.
    target_compile_options(whitehole_app PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()
whitehole_optimize(whitehole_app)

# Windows resources: icon, version info, Common Controls v6 + DPI manifest.
set(WHITEHOLE_RES_FILES "")
if(WHITEHOLE_HAVE_WIN32_GUI)
    enable_language(RC)
    set(WHITEHOLE_VERSION_COMMA "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
    set(WHITEHOLE_VERSION_DOT "${PROJECT_VERSION}.0")
    # The resource compiler is a native Windows binary, so keep every path it
    # sees relative to the build directory.
    set(WHITEHOLE_RES_DIR "generated")
    configure_file(cpp/res/whitehole.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/whitehole.rc" @ONLY)
    file(COPY cpp/res/whitehole.ico cpp/res/whitehole.manifest DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(WHITEHOLE_RES_FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/whitehole.rc")
endif()

# ---------------------------------------------------------------------------
# Runtime data
# Both executables land in the same output directory, so a POST_BUILD copy on
# each target raced with itself under a parallel build (-j) and intermittently
# failed with "Error copying directory". One shared target copies the data
# once; each executable only depends on it.
# ---------------------------------------------------------------------------
if(CMAKE_CONFIGURATION_TYPES)
    set(WHITEHOLE_RUNTIME_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>")
else()
    set(WHITEHOLE_RUNTIME_DIR "${CMAKE_BINARY_DIR}")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    add_custom_target(whitehole-runtime-data
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "${WHITEHOLE_RUNTIME_DIR}/data"
        COMMENT "Copying runtime data into ${WHITEHOLE_RUNTIME_DIR}/data"
        VERBATIM)
else()
    add_custom_target(whitehole-runtime-data)
endif()

function(whitehole_copy_data target)
    add_dependencies(${target} whitehole-runtime-data)
endfunction()

# ---------------------------------------------------------------------------
# Console executable: CLI, scripting, CI
# ---------------------------------------------------------------------------
add_executable(whitehole-pro-console cpp/src/main.cpp $<TARGET_OBJECTS:whitehole_app>)
target_link_libraries(whitehole-pro-console PRIVATE whitehole_core whitehole_win32)
if(WHITEHOLE_HAVE_WIN32_GUI)
    target_link_libraries(whitehole-pro-console PRIVATE whitehole_imgui)
endif()
if(MSVC)
    target_compile_options(whitehole-pro-console PRIVATE /utf-8)
endif()
whitehole_optimize(whitehole-pro-console)
whitehole_copy_data(whitehole-pro-console)
if(WHITEHOLE_RES_FILES)
    target_sources(whitehole-pro-console PRIVATE ${WHITEHOLE_RES_FILES})
endif()
if(MSVC AND WHITEHOLE_RES_FILES)
    set_target_properties(whitehole-pro-console PROPERTIES LINK_FLAGS "/MANIFEST:NO")
endif()

# ---------------------------------------------------------------------------
# Windowed executable: double-click friendly, no console flash
# ---------------------------------------------------------------------------
if(WHITEHOLE_HAVE_WIN32_GUI AND WHITEHOLE_BUILD_GUI)
    add_executable(whitehole-pro WIN32 cpp/src/winmain.cpp $<TARGET_OBJECTS:whitehole_app>)
    target_link_libraries(whitehole-pro PRIVATE whitehole_core whitehole_win32 whitehole_imgui)
    if(NOT MSVC)
        # GNU toolchains need this to accept the wWinMain entry point.
        target_link_options(whitehole-pro PRIVATE -municode)
    endif()
    whitehole_optimize(whitehole-pro)
    whitehole_copy_data(whitehole-pro)
    if(WHITEHOLE_RES_FILES)
    target_sources(whitehole-pro PRIVATE ${WHITEHOLE_RES_FILES})
endif()
if(MSVC AND WHITEHOLE_RES_FILES)
    set_target_properties(whitehole-pro PROPERTIES LINK_FLAGS "/MANIFEST:NO")
endif()
    install(TARGETS whitehole-pro RUNTIME DESTINATION bin)
endif()

install(TARGETS whitehole-pro-console RUNTIME DESTINATION bin)
install(DIRECTORY data DESTINATION share/whitehole-pro)

if(WHITEHOLE_BUILD_TESTS)
    enable_testing()
    add_executable(whitehole_core_tests cpp/tests/core_tests.cpp)
    target_link_libraries(whitehole_core_tests PRIVATE whitehole_core)
    target_compile_definitions(whitehole_core_tests PRIVATE
        WHITEHOLE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME whitehole_core_tests COMMAND whitehole_core_tests)
endif()
