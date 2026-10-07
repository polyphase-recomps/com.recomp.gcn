# A recompiled GameCube game: the static library the editor addon links, and a standalone runner.
#
#   cmake -S Runtime/recomp/host -B <build> -G Ninja -DCMAKE_C_COMPILER=clang-cl
#         -DGCN_HLE_DIR=<gcn_hle_build.py --out> -DGCN_RECOMP_DIR=<GcnRecomp --out>
#         -DGCN_HLE_NAME=sfarecomp -DGCN_RECOMP_NAME=sfa
#
#  <name>_recomp (static library): everything of the game - the recompiled functions (GcnRecomp),
#    the HLE module (the runtime's SDK replacement, wasm2c), its glue (gen_hle_glue.py) and the
#    recomp runtime (recomp_gcn.c). It needs com.recomp.gcn's Source/Gcn (backend, GPU, wasm-rt)
#    from whoever links it: the addon (build_recomp.ps1 publishes it there) or the runner.
#  <hle name>_runner (GCN_RECOMP_RUNNER): Runtime/host/gcn_main.c with that library, the same
#    command line as the decomp runner (test harness).
cmake_minimum_required(VERSION 3.20)
project(gcn_recomp C)

set(GCN_HLE_DIR "" CACHE PATH "gcn_hle_build.py output (the HLE module + <name>_glue.c)")
set(GCN_RECOMP_DIR "" CACHE PATH "GcnRecomp output (funcs_*.c, functable.c, game.c)")
set(GCN_HLE_NAME "" CACHE STRING "HLE module name (e.g. sfarecomp)")
set(GCN_RECOMP_NAME "" CACHE STRING "game build name (e.g. sfa); the library is <name>_recomp")
if(NOT GCN_HLE_DIR OR NOT GCN_RECOMP_DIR OR NOT GCN_HLE_NAME)
    message(FATAL_ERROR "set GCN_HLE_DIR, GCN_RECOMP_DIR and GCN_HLE_NAME")
endif()
if(NOT GCN_RECOMP_NAME)
    string(REGEX REPLACE "recomp$" "" GCN_RECOMP_NAME "${GCN_HLE_NAME}")
endif()
option(GCN_RECOMP_RUNNER "also build the standalone runner" ON)
option(GCNR_FMA "fused multiply-add like Gekko (OFF: like the decomp build, for comparisons)" ON)
option(GCNR_CHECKED "log unproven accesses to hardware registers" OFF)
option(GCNR_WATCH "store watchpoint support (GCNR_WATCH=<hex address>)" OFF)

set(GCN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(GCN_SOURCE "${GCN_ROOT}/Source/Gcn")
set(GCN_RECOMP "${GCN_ROOT}/Runtime/recomp")

file(GLOB HLE_SOURCES "${GCN_HLE_DIR}/${GCN_HLE_NAME}_guest_*.c")
file(GLOB RECOMP_FUNCS "${GCN_RECOMP_DIR}/funcs_*.c")
set(RECOMP_GAME_SOURCES ${RECOMP_FUNCS} "${GCN_RECOMP_DIR}/functable.c" "${GCN_RECOMP_DIR}/game.c")
set(RECOMP_GLUE_SOURCES ${HLE_SOURCES} "${GCN_HLE_DIR}/${GCN_HLE_NAME}_glue.c" "${GCN_RECOMP}/recomp_gcn.c")

set(LIB "${GCN_RECOMP_NAME}_recomp")
add_library(${LIB} STATIC ${RECOMP_GAME_SOURCES} ${RECOMP_GLUE_SOURCES})
target_include_directories(${LIB} PRIVATE "${GCN_RECOMP}/include" "${GCN_RECOMP_DIR}" "${GCN_HLE_DIR}" "${GCN_SOURCE}")
target_compile_definitions(${LIB} PRIVATE GCNW_RECOMP=1 GCNR_FMA=$<BOOL:${GCNR_FMA}> GCNR_CHECKED=$<BOOL:${GCNR_CHECKED}>
                                          GCNR_WATCH=$<BOOL:${GCNR_WATCH}>)
if(MSVC)
    # the generated code at full optimisation whatever the build type (it is the game)
    target_compile_options(${LIB} PRIVATE /O2 /bigobj)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${LIB} PRIVATE -Wno-everything -gline-tables-only)
    else()
        target_compile_options(${LIB} PRIVATE /W0)
    endif()
else()
    target_compile_options(${LIB} PRIVATE -O2 -w)
endif()

if(GCN_RECOMP_RUNNER)
    add_executable(gcn_recomp_runner
        ${GCN_ROOT}/Runtime/host/gcn_main.c
        ${GCN_SOURCE}/gcnw_backend.c
        ${GCN_SOURCE}/gcnw_rt.c
        ${GCN_SOURCE}/gcn_gpu.c
        ${GCN_SOURCE}/gcn_raster.c
        ${GCN_SOURCE}/gcn_thp.c
        ${GCN_SOURCE}/gcn_overlay.c
        ${GCN_SOURCE}/gcn_disc.c)
    target_include_directories(gcn_recomp_runner PRIVATE "${GCN_SOURCE}" "${GCN_HLE_DIR}")
    target_compile_definitions(gcn_recomp_runner PRIVATE GCN_GUEST_MODULE=gcnw_module_${GCN_HLE_NAME})
    target_link_libraries(gcn_recomp_runner PRIVATE ${LIB})
    set_target_properties(gcn_recomp_runner PROPERTIES OUTPUT_NAME "${GCN_HLE_NAME}_runner")
    if(MSVC)
        set_source_files_properties(${GCN_SOURCE}/gcn_raster.c ${GCN_SOURCE}/gcn_gpu.c ${GCN_SOURCE}/gcn_thp.c PROPERTIES COMPILE_OPTIONS "/O2")
        target_compile_options(gcn_recomp_runner PRIVATE /W0)
        if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
            target_compile_options(gcn_recomp_runner PRIVATE -Wno-everything -gline-tables-only)
        endif()
        # line tables and a fixed load address: crash / hang stacks come out symbolized
        target_link_options(gcn_recomp_runner PRIVATE /STACK:16777216 /DEBUG /DYNAMICBASE:NO)
    else()
        target_compile_options(gcn_recomp_runner PRIVATE -w)
        target_link_libraries(gcn_recomp_runner PRIVATE m)
    endif()
endif()
