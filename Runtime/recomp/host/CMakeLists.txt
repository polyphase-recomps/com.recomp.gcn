# Standalone runner of a recompiled GameCube game (test harness; the editor uses the addon).
#
#   cmake -S Runtime/recomp/host -B <build> -G Ninja -DCMAKE_C_COMPILER=clang-cl
#         -DGCN_HLE_DIR=<gcn_hle_build.py --out> -DGCN_RECOMP_DIR=<GcnRecomp --out> -DGCN_HLE_NAME=sfarecomp
#
# The same runner as Runtime/host (gcn_main.c, the same command line), with the recomp game:
# the HLE module (wasm2c), its glue (gen_hle_glue.py) and the recompiled functions.
cmake_minimum_required(VERSION 3.20)
project(gcn_recomp_runner C)

set(GCN_HLE_DIR "" CACHE PATH "gcn_hle_build.py output (the HLE module + <name>_glue.c)")
set(GCN_RECOMP_DIR "" CACHE PATH "GcnRecomp output (funcs_*.c, functable.c, game.c)")
set(GCN_HLE_NAME "" CACHE STRING "HLE module name (e.g. sfarecomp)")
if(NOT GCN_HLE_DIR OR NOT GCN_RECOMP_DIR OR NOT GCN_HLE_NAME)
    message(FATAL_ERROR "set GCN_HLE_DIR, GCN_RECOMP_DIR and GCN_HLE_NAME")
endif()
option(GCNR_FMA "fused multiply-add like Gekko (OFF: like the decomp build, for comparisons)" ON)
option(GCNR_CHECKED "log unproven accesses to hardware registers" OFF)
option(GCNR_WATCH "store watchpoint support (GCNR_WATCH=<hex address>)" OFF)

set(GCN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(GCN_SOURCE "${GCN_ROOT}/Source/Gcn")
set(GCN_RECOMP "${GCN_ROOT}/Runtime/recomp")

file(GLOB HLE_SOURCES "${GCN_HLE_DIR}/${GCN_HLE_NAME}_guest_*.c")
file(GLOB RECOMP_FUNCS "${GCN_RECOMP_DIR}/funcs_*.c")

# the recompiled game: big generated C, its own library so its flags stay apart
add_library(gcn_recomp_game STATIC ${RECOMP_FUNCS} "${GCN_RECOMP_DIR}/functable.c" "${GCN_RECOMP_DIR}/game.c")
target_include_directories(gcn_recomp_game PRIVATE "${GCN_RECOMP}/include" "${GCN_RECOMP_DIR}")
target_compile_definitions(gcn_recomp_game PRIVATE GCNR_FMA=$<BOOL:${GCNR_FMA}> GCNR_CHECKED=$<BOOL:${GCNR_CHECKED}> GCNR_WATCH=$<BOOL:${GCNR_WATCH}>)

add_executable(gcn_recomp_runner
    ${GCN_ROOT}/Runtime/host/gcn_main.c
    ${GCN_SOURCE}/gcnw_backend.c
    ${GCN_SOURCE}/gcnw_rt.c
    ${GCN_SOURCE}/gcn_gpu.c
    ${GCN_SOURCE}/gcn_raster.c
    ${GCN_SOURCE}/gcn_thp.c
    ${GCN_SOURCE}/gcn_overlay.c
    ${GCN_SOURCE}/gcn_disc.c
    ${HLE_SOURCES}
    "${GCN_HLE_DIR}/${GCN_HLE_NAME}_glue.c"
    "${GCN_RECOMP}/recomp_gcn.c")
target_include_directories(gcn_recomp_runner PRIVATE "${GCN_SOURCE}" "${GCN_HLE_DIR}" "${GCN_RECOMP}/include")
target_compile_definitions(gcn_recomp_runner PRIVATE GCN_GUEST_MODULE=gcnw_module_${GCN_HLE_NAME} GCNW_RECOMP=1)
target_link_libraries(gcn_recomp_runner PRIVATE gcn_recomp_game)
set_target_properties(gcn_recomp_runner PROPERTIES OUTPUT_NAME "${GCN_HLE_NAME}_runner")
if(MSVC)
    set_source_files_properties(${GCN_SOURCE}/gcn_raster.c ${GCN_SOURCE}/gcn_gpu.c ${GCN_SOURCE}/gcn_thp.c PROPERTIES COMPILE_OPTIONS "/O2")
    target_compile_options(gcn_recomp_runner PRIVATE /W0 /bigobj)
    target_compile_options(gcn_recomp_game PRIVATE /O2 /bigobj)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(gcn_recomp_runner PRIVATE -Wno-everything)
        target_compile_options(gcn_recomp_game PRIVATE -Wno-unused-label -Wno-unused-variable)
    endif()
    target_link_options(gcn_recomp_runner PRIVATE /STACK:16777216)
    # line tables and a fixed load address: crash / hang stacks symbolize with llvm-symbolizer
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(gcn_recomp_runner PRIVATE -gline-tables-only)
        target_compile_options(gcn_recomp_game PRIVATE -gline-tables-only)
    else()
        target_compile_options(gcn_recomp_runner PRIVATE /Zi)
        target_compile_options(gcn_recomp_game PRIVATE /Zi)
    endif()
    target_link_options(gcn_recomp_runner PRIVATE /DEBUG /DYNAMICBASE:NO)
else()
    target_compile_options(gcn_recomp_runner PRIVATE -w)
    target_compile_options(gcn_recomp_game PRIVATE -O2 -Wno-unused-label)
    target_link_libraries(gcn_recomp_runner PRIVATE m)
endif()
