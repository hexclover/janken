add_executable( server server.c ${PROJECT_SOURCE_DIR}/lib/common.h )
target_link_libraries ( server common Threads::Threads )
