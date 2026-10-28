# Black (original Xbox), statically recompiled, as a macOS application: Black.app, one binary with SDL3 and MoltenVK.
# The x86 code is recompiled first (build.sh): this build takes generated/game as it is, with the SDK's portable runtime,
# the game's sources in src and the platform host in macos.
#
#   cmake -S . -B out/build/mac -DCMAKE_BUILD_TYPE=Release
cmake_minimum_required(VERSION 3.22)
project(black_macos LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(REPO ${CMAKE_CURRENT_SOURCE_DIR})
set(MAC ${REPO}/macos)   # the window, the event loop, the folders, the input
set(GAME ${REPO}/src)    # the game's hooks, sound, Direct3D 8 and settings

# the SDK: sdk/ in the repo, a checkout beside it, else downloaded
include(FetchContent)
if(EXISTS ${REPO}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${REPO}/sdk)
elseif(EXISTS ${REPO}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    get_filename_component(SDK ${REPO}/../XboxXBE-Recomp ABSOLUTE)
else()
    # pinned: the SDK's main moved to game code built into a DLL at first start, which this port does not take
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git
                         GIT_TAG 99078b119b5d5e0d1d6afb1390e29437c0c984c0)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()

set(XBE_GENERATED ${REPO}/generated/game CACHE PATH "the recompiled code of default.xbe")
set(XBE_IMAGE ${REPO}/assets/default.xbe CACHE FILEPATH "the xbe it was recompiled from")
set(GEN ${XBE_GENERATED})
set(XBE ${XBE_IMAGE})
if(NOT EXISTS ${GEN}/x86_func_mapping.cpp OR NOT EXISTS ${XBE})
    message(FATAL_ERROR "No recompiled code: put the game in assets/ and run build.sh, it writes generated/game")
endif()
file(STRINGS ${REPO}/config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
math(EXPR last "${GAME_PARTITIONS} - 1")
set(GEN_SOURCES ${GEN}/x86_func_mapping.cpp)
foreach(i RANGE ${last})
    list(APPEND GEN_SOURCES ${GEN}/x86_recomp.${i}.cpp)
endforeach()
file(SHA1 ${XBE} xbe_sha1)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp CONTENT "extern const char kImageSha1[] = \"${xbe_sha1}\";\n")
set(BUILD_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_EXECUTABLE)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD WORKING_DIRECTORY ${REPO}
                    OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
    if(NOT failed AND head)
        set(BUILD_COMMIT ${head})
    endif()
endif()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/build_info.h CONTENT "#define BLACK_COMMIT \"${BUILD_COMMIT}\"\n")

# dependencies: SDL3 from the checkout build.sh makes, the rest fetched
set(SDL_DIR ${REPO}/third_party/SDL)
if(NOT EXISTS ${SDL_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "SDL3 is missing from ${SDL_DIR}: run build.sh, it clones it")
endif()
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/SDL EXCLUDE_FROM_ALL)

set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG 16.0.0 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glslang tomlplusplus)

# the SDK's runtime without its Windows parts and those the game replaces (src/sound, src/d3d); xnet is Winsock and
# this build has no system link, so it is out too
file(GLOB RUNTIME_SOURCES ${SDK}/runtime/*/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX
     "/runtime/app/|/core/input\\.cpp$|/gpu/(d3d12|render_thread)\\.cpp$|/audio/xaudio2\\.cpp$|/xdk/(dsound|d3d8|xnet)\\.cpp$")
file(GLOB GAME_HOST_SOURCES CONFIGURE_DEPENDS ${GAME}/*/*.cpp)
file(GLOB HOST_SOURCES CONFIGURE_DEPENDS ${MAC}/*.cpp)

add_executable(Black MACOSX_BUNDLE ${RUNTIME_SOURCES} ${GAME_HOST_SOURCES} ${HOST_SOURCES} ${GEN_SOURCES}
               ${CMAKE_CURRENT_BINARY_DIR}/image_sha1.cpp ${CMAKE_CURRENT_BINARY_DIR}/europe_patch.cpp)
set_target_properties(Black PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME "Black" MACOSX_BUNDLE_GUI_IDENTIFIER "dev.shei.black"
                      MACOSX_BUNDLE_BUNDLE_VERSION "1.0" MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0")
# compat/ has the macOS shims; the SDK's harness compat carries the Windows names its portable runtime still spells out
target_include_directories(Black PRIVATE ${MAC}/compat ${SDK}/harness/compat ${MAC} ${GAME} ${SDK}/runtime ${GEN}
                           ${CMAKE_CURRENT_BINARY_DIR} ${glslang_SOURCE_DIR})
target_compile_definitions(Black PRIVATE GL_SILENCE_DEPRECATION)
target_compile_options(Black PRIVATE -fno-strict-aliasing -fsigned-char -Wno-deprecated-declarations)
# src/common/europe.patch as a C array: the ELF build incbins it with inline asm, which the Mach-O assembler cannot do
file(READ ${REPO}/src/common/europe.patch EUROPE_HEX HEX)
string(LENGTH "${EUROPE_HEX}" EUROPE_HEX_LEN)
math(EXPR EUROPE_SIZE "${EUROPE_HEX_LEN} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," EUROPE_BYTES "${EUROPE_HEX}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/europe_patch.cpp
     "// generated from src/common/europe.patch\n"
     "#include <cstddef>\n"
     "#include <cstdint>\n"
     "extern \"C\" const uint8_t kEuropePatch[] = {${EUROPE_BYTES}};\n"
     "extern \"C\" const size_t kEuropePatchSize = ${EUROPE_SIZE};\n")

set(ALL_HOST ${RUNTIME_SOURCES} ${GAME_HOST_SOURCES} ${HOST_SOURCES})
set_source_files_properties(${ALL_HOST} PROPERTIES COMPILE_FLAGS "-include ${MAC}/compat/crt.h")
set_source_files_properties(${GEN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-O2;-w;-fno-strict-aliasing;-fsigned-char")
set_source_files_properties(${MAC}/vk_shader.cpp PROPERTIES COMPILE_DEFINITIONS ENABLE_HLSL)
find_library(MOLTENVK_LIB MoltenVK PATHS /usr/local/lib /opt/homebrew/lib)
find_library(VULKAN_LIB vulkan PATHS /usr/local/lib /opt/homebrew/lib)
find_path(VULKAN_INCLUDE vulkan/vulkan.h PATHS /usr/local/include /opt/homebrew/include)
if(NOT MOLTENVK_LIB OR NOT VULKAN_LIB OR NOT VULKAN_INCLUDE)
    message(FATAL_ERROR "Vulkan is missing: brew install molten-vk, or put libvulkan/libMoltenVK in /usr/local/lib and "
                        "vulkan/ in /usr/local/include")
endif()
target_include_directories(Black SYSTEM PRIVATE ${VULKAN_INCLUDE})
# the loader resolves the ICD through its own search; linking it keeps one copy of the entry points in the bundle
target_link_libraries(Black PRIVATE SDL3::SDL3-static tomlplusplus::tomlplusplus glslang glslang-default-resource-limits
                      ${VULKAN_LIB} ${MOLTENVK_LIB} "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation"
                      "-framework CoreAudio" "-framework AudioToolbox" "-framework AVFoundation" "-framework Metal"
                      "-framework QuartzCore" "-framework GameController" "-framework CoreHaptics" "-framework UniformTypeIdentifiers")
set_target_properties(Black PROPERTIES BUILD_RPATH "@executable_path/../Frameworks")