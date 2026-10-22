# coopfuzz target for cdmp's quest sharing (quest_target.cpp). Game-free, builds natively.
#   cmake -S tests/fuzz -B build-fuzz -DCOOPFUZZ_DIR=<coop-anything>/tools/fuzz
#   cmake --build build-fuzz --parallel 2
#   build-fuzz/coopfuzz/coopfuzz --target cdmp_quests --count 500
# coopfuzz (MIT) comes from the coop-anything hub: https://github.com/cranebeaker/coop-anything/tree/main/tools/fuzz
cmake_minimum_required(VERSION 3.16)
project(cdmp_fuzz LANGUAGES CXX)
if(NOT COOPFUZZ_DIR)
  message(FATAL_ERROR "set COOPFUZZ_DIR to coop-anything's tools/fuzz")
endif()
set(CDMP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(COOPFUZZ_TARGET_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/quest_target.cpp CACHE STRING "" FORCE)
set(COOPFUZZ_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${COOPFUZZ_DIR} coopfuzz)
target_sources(coopfuzz PRIVATE ${CDMP_ROOT}/src/common/protocol.cpp ${CDMP_ROOT}/src/common/reliable.cpp
                               ${CDMP_ROOT}/third_party/cofr/cofr.cpp)
target_include_directories(coopfuzz PRIVATE ${CDMP_ROOT}/src ${CDMP_ROOT}/third_party/cofr)
set_target_properties(coopfuzz PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
