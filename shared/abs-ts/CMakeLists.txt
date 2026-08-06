cmake_minimum_required(VERSION 3.28...3.30)

add_library(abs-ts OBJECT)
add_library(OBS::abs-ts ALIAS abs-ts)

target_sources(abs-ts PRIVATE abs-ts.c PUBLIC abs-ts.h)

target_include_directories(abs-ts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(abs-ts PUBLIC OBS::libobs OBS::caption)

set_target_properties(abs-ts PROPERTIES FOLDER deps POSITION_INDEPENDENT_CODE TRUE)
