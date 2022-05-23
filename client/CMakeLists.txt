add_executable( client client.c ${PROJECT_SOURCE_DIR}/lib/common.h )
target_link_libraries( client common Threads::Threads ${CURSES_LIBRARIES} menu panel )
