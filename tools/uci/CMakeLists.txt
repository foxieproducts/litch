cmake_minimum_required(VERSION 3.20)
project(litch_uci LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

set(BUILD_TESTING OFF CACHE BOOL "Engine tests run in their standalone owner" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../lib/litch-max" "litch-max")
set(LITCH_FOXIE_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../lib/foxie_embedded/include")
foreach(header IN ITEMS fixed_string.hpp format.hpp)
  file(SHA256 "${LITCH_FOXIE_INCLUDE}/foxie_embedded/${header}" owner_hash)
  file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../../lib/litch-max/third_party/foxie_embedded/include/foxie_embedded/${header}" bundled_hash)
  if(NOT owner_hash STREQUAL bundled_hash)
    message(FATAL_ERROR "Litch-Max portable header differs from its owner: ${header}")
  endif()
endforeach()
target_include_directories(litch_max BEFORE PUBLIC "${LITCH_FOXIE_INCLUDE}")

add_executable(litch_uci
  uci_main.cpp
  hint_benchmark.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/async_think_worker.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../lib/foxie_embedded/src/logger.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../lib/foxie_embedded/src/millis.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/chess_strings.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/game_persistence_coordinator_vs_cpu_helpers.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/vs_cpu_rng.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/litch_max_engine_classic.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/litch_max_engine.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/litch_max_engine_benchmark.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/engine_benchmark.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/litch_max_engine_duck.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/litch_max_engine_uci_move.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/progress_watchdog.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/progress_watchdog_platform.cpp
)

target_include_directories(litch_uci PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../include
  ${CMAKE_CURRENT_LIST_DIR}/../../lib/foxie_embedded/include
  ${CMAKE_CURRENT_LIST_DIR}/../../lib/foxie_embedded/src
)

target_link_libraries(litch_uci PRIVATE litch_max::litch_max Threads::Threads)
