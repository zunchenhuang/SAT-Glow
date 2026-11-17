cmake_minimum_required(VERSION 3.16)
project(satglow C CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SATGLOW_GUI "Build the OpenGL visualizer (needs GLFW)" ON)

find_package(Threads REQUIRED)

# --- The three solvers ------------------------------------------------------
# Each solver is built twice: with the satglow hooks (used everywhere), and
# with all hooks compiled out (-DSG_DISABLE_HOOKS), which is the baseline
# that satglow-check-base runs for overhead measurements.
file(GLOB CADICAL_SOURCES ${CMAKE_SOURCE_DIR}/third_party/cadical/src/*.cpp)
list(FILTER CADICAL_SOURCES EXCLUDE REGEX "/(cadical|mobical)\\.cpp$")
file(GLOB KISSAT_SOURCES ${CMAKE_SOURCE_DIR}/third_party/kissat/src/*.c)
set(MINISAT_SOURCES
  third_party/minisat/minisat/core/Solver.cc
  third_party/minisat/minisat/simp/SimpSolver.cc
  third_party/minisat/minisat/utils/System.cc)

function(add_solvers suffix)
  add_library(cadical${suffix} STATIC ${CADICAL_SOURCES})
  target_include_directories(cadical${suffix}
    PUBLIC ${CMAKE_SOURCE_DIR}/third_party/cadical/src ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(cadical${suffix} PRIVATE NBUILD NDEBUG NCLOSEFROM ${ARGN})
  target_compile_options(cadical${suffix} PRIVATE -w)

  add_library(kissat${suffix} STATIC ${KISSAT_SOURCES})
  target_include_directories(kissat${suffix}
    PUBLIC ${CMAKE_SOURCE_DIR}/third_party/kissat/src ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(kissat${suffix} PRIVATE NDEBUG QUIET ${ARGN})
  target_compile_options(kissat${suffix} PRIVATE -w)
  set_target_properties(kissat${suffix} PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
  target_link_libraries(kissat${suffix} PUBLIC m)

  add_library(minisat${suffix} STATIC ${MINISAT_SOURCES})
  target_include_directories(minisat${suffix} SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party/minisat)
  target_include_directories(minisat${suffix} PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(minisat${suffix}
    PUBLIC MINISAT_NO_ZLIB __STDC_LIMIT_MACROS __STDC_FORMAT_MACROS PRIVATE ${ARGN})
  target_compile_options(minisat${suffix} PRIVATE -w)

  # solver side, no graphics
  add_library(sgcore${suffix} STATIC
    src/cnf.cpp
    src/shared.cpp
    src/solver.cpp
    src/backend_cadical.cpp
    src/backend_kissat.cpp
    src/backend_minisat.cpp
    src/backend_replay.cpp
    src/trace.cpp
    src/metrics.cpp
    src/graph.cpp
    src/clusters.cpp)
  target_include_directories(sgcore${suffix} PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(sgcore${suffix}
    PUBLIC cadical${suffix} kissat${suffix} minisat${suffix} Threads::Threads)
  target_compile_options(sgcore${suffix} PRIVATE -Wall -Wextra)
endfunction()

add_solvers("")
add_solvers("_base" SG_DISABLE_HOOKS)

add_executable(satglow-check src/check_main.cpp)
target_link_libraries(satglow-check PRIVATE sgcore)

add_executable(satglow-check-base src/check_main.cpp)
target_compile_definitions(satglow-check-base PRIVATE SATGLOW_BASE)
target_link_libraries(satglow-check-base PRIVATE sgcore_base)

add_executable(satglow-metrics src/metrics_main.cpp)
target_link_libraries(satglow-metrics PRIVATE sgcore)

# --- visualizer --------------------------------------------------------------
if(SATGLOW_GUI)
  find_package(glfw3 3.3 REQUIRED)

  add_library(glad STATIC third_party/glad/src/glad.c)
  target_include_directories(glad PUBLIC third_party/glad/include)
  target_link_libraries(glad PUBLIC ${CMAKE_DL_LIBS})

  add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
  target_link_libraries(imgui PUBLIC glfw ${CMAKE_DL_LIBS})

  add_executable(satglow
    src/main.cpp
    src/bloom.cpp
    src/intro.cpp
    src/layout.cpp
    src/logo.cpp
    src/recorder.cpp
    src/shockwave.cpp)
  target_link_libraries(satglow PRIVATE sgcore imgui glad glfw Threads::Threads)
  target_compile_options(satglow PRIVATE -Wall -Wextra)
endif()
