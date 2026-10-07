# GcnRecomp: the GameCube (Gekko PowerPC) recompiler, DOL -> C. Built by
# Runtime/tools/recomp/build_recomp.ps1 into Runtime/build/gcnrecomp.
#
#   cmake -S Runtime/recomp/tool -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build <build>
cmake_minimum_required(VERSION 3.16)
project(GcnRecomp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(gcnrecomp_core STATIC gekko.cpp program.cpp analysis.cpp cgen.cpp)
target_include_directories(gcnrecomp_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(GcnRecomp main.cpp)
target_link_libraries(GcnRecomp PRIVATE gcnrecomp_core)

# decodes a whole DOL for test/compare_objdump.py
add_executable(gcnrecomp_decode_test test/decode_test.cpp)
target_link_libraries(gcnrecomp_decode_test PRIVATE gcnrecomp_core)

if(MSVC)
    target_compile_definitions(gcnrecomp_core PUBLIC _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(gcnrecomp_decode_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
