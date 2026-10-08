# Standalone runner of a com.recomp.gcn game (test harness; the editor uses the addon).
#
#   cmake -S Runtime/host -B <build> -G Ninja -DGCN_GUEST_DIR=<generated C> -DGCN_GUEST_NAME=sfa
#
# GCN_GUEST_DIR holds <name>_guest*.c from Runtime/tools/gcn_build.py.
cmake_minimum_required(VERSION 3.20)
project(gcn_runner C)

set(GCN_GUEST_DIR "" CACHE PATH "wasm2c output of the game (gcn_build.py)")
set(GCN_GUEST_NAME "" CACHE STRING "build name of the game (gcn_game.json \"name\")")
if(NOT GCN_GUEST_DIR OR NOT GCN_GUEST_NAME)
    message(FATAL_ERROR "set GCN_GUEST_DIR and GCN_GUEST_NAME")
endif()
set(GCN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/Gcn")

file(GLOB GUEST_SOURCES "${GCN_GUEST_DIR}/${GCN_GUEST_NAME}_guest_*.c")
add_executable(gcn_runner
    gcn_main.c
    ${GCN_SOURCE}/gcnw_backend.c
    ${GCN_SOURCE}/gcnw_rt.c
    ${GCN_SOURCE}/gcn_gpu.c
    ${GCN_SOURCE}/gcn_raster.c
    ${GCN_SOURCE}/gcn_vk.c
    ${GCN_SOURCE}/gcn_thp.c
    ${GCN_SOURCE}/gcn_overlay.c
    ${GCN_SOURCE}/gcn_disc.c
    ${GUEST_SOURCES})
target_include_directories(gcn_runner PRIVATE "${GCN_SOURCE}" "${GCN_GUEST_DIR}")
# the host GPU renderer (gcn_vk.c) needs the Vulkan headers (the Vulkan SDK); without them it
# compiles to nothing and the software rasteriser draws
if(DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Include/vulkan/vulkan.h")
    target_include_directories(gcn_runner PRIVATE "$ENV{VULKAN_SDK}/Include")
elseif(DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/include/vulkan/vulkan.h")
    target_include_directories(gcn_runner PRIVATE "$ENV{VULKAN_SDK}/include")
endif()
target_compile_definitions(gcn_runner PRIVATE GCN_GUEST_MODULE=gcnw_module_${GCN_GUEST_NAME})
# debugging: every guest store checked against GCN_WATCH_WRITE (slow)
option(GCN_WATCH_WRITES "store watchpoint (GCN_WATCH_WRITE=lo:hi[:value])" OFF)
if(GCN_WATCH_WRITES)
    target_compile_definitions(gcn_runner PRIVATE GCNW_WATCH_WRITES=1)
endif()
set_target_properties(gcn_runner PROPERTIES OUTPUT_NAME "${GCN_GUEST_NAME}_runner")
if(MSVC)
    # the GPU emulation is the hot path: full optimisation there, whatever the build type
    # (the generated game files stay at the build type's level, they take long to compile)
    set_source_files_properties(${GCN_SOURCE}/gcn_raster.c ${GCN_SOURCE}/gcn_gpu.c ${GCN_SOURCE}/gcn_thp.c PROPERTIES COMPILE_OPTIONS "/O2")
    # wasm2c output: big functions, implicit conversions it relies on
    target_compile_options(gcn_runner PRIVATE /W0 /bigobj)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(gcn_runner PRIVATE -Wno-everything)
    endif()
    target_link_options(gcn_runner PRIVATE /STACK:16777216)
else()
    target_compile_options(gcn_runner PRIVATE -w)
    target_link_libraries(gcn_runner PRIVATE m)
endif()
