cmake_minimum_required( VERSION 3.10 )

project( cli_game LANGUAGES C )

set( CMAKE_C_STANDARD 11 )
set( CMAKE_C_STANDARD_REQUIRED ON )

add_compile_options( -O2 -Wall -pedantic )
if ( CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "10.1" )
  add_compile_options( -fanalyzer )
endif ()

set( CURSES_NEED_NCURSES True )
find_package( Curses REQUIRED )
include_directories( ${CURSES_INCLUDE_DIR} )

set( CMAKE_THREAD_PREFER_PTHREAD TRUE )
set( THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package( Threads REQUIRED )

set( CMAKE_EXPORT_COMPILE_COMMANDS ON )

set( CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin )

include_directories( ${PROJECT_SOURCE_DIR} )

add_subdirectory( lib/ )
add_subdirectory( client/ )
add_subdirectory( server/ )
