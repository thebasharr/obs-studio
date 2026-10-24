cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Widgets Network)

add_library(station-studio MODULE)
add_library(OBS::station-studio ALIAS station-studio)

target_sources(
  station-studio
  PRIVATE
    core/scheduler-engine.cpp
    core/scheduler-engine.hpp
    core/station-schedule.cpp
    core/station-schedule.hpp
    hls-server.cpp
    hls-server.hpp
    outputs-dialog.cpp
    outputs-dialog.hpp
    outputs-manager.cpp
    outputs-manager.hpp
    playout-controller.cpp
    playout-controller.hpp
    plugin-main.cpp
    slot-dialog.cpp
    slot-dialog.hpp
    station-dock.cpp
    station-dock.hpp
    station-state.cpp
    station-state.hpp
)

target_include_directories(station-studio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_options(station-studio PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_link_libraries(station-studio PRIVATE OBS::frontend-api OBS::libobs Qt::Widgets Qt::Network)

set_target_properties_obs(station-studio PROPERTIES FOLDER plugins/station-studio PREFIX "" AUTOMOC ON)

if(ENABLE_TESTS OR BUILD_TESTING)
  add_executable(station-studio-core-tests)
  target_sources(
    station-studio-core-tests
    PRIVATE tests/test-station-core.cpp core/scheduler-engine.cpp core/station-schedule.cpp
  )
  target_include_directories(station-studio-core-tests PRIVATE core)
  target_compile_features(station-studio-core-tests PRIVATE cxx_std_17)
  add_test(NAME station-studio-core-tests COMMAND station-studio-core-tests)
endif()
