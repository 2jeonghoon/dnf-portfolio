cmake_minimum_required(VERSION 3.16)

project(dungeon_portfolio_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

execute_process(
  COMMAND mysql_config --cflags
  OUTPUT_VARIABLE MYSQL_CFLAGS
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE MYSQL_CFLAGS_RESULT
)
if(NOT MYSQL_CFLAGS_RESULT EQUAL 0)
  message(FATAL_ERROR "mysql_config --cflags failed")
endif()

execute_process(
  COMMAND mysql_config --libs
  OUTPUT_VARIABLE MYSQL_LIBS
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE MYSQL_LIBS_RESULT
)
if(NOT MYSQL_LIBS_RESULT EQUAL 0)
  message(FATAL_ERROR "mysql_config --libs failed")
endif()

separate_arguments(MYSQL_CFLAGS_LIST UNIX_COMMAND "${MYSQL_CFLAGS}")
separate_arguments(MYSQL_LIBS_LIST UNIX_COMMAND "${MYSQL_LIBS}")

add_executable(dungeon_portfolio_server
  src/config.cpp
  src/database.cpp
  src/main.cpp
  src/protocol.cpp
  src/server.cpp
  src/world.cpp
)

target_include_directories(dungeon_portfolio_server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${LIBURING_INCLUDE_DIRS}
)

target_compile_options(dungeon_portfolio_server PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  ${LIBURING_CFLAGS_OTHER}
  ${MYSQL_CFLAGS_LIST}
)

target_link_libraries(dungeon_portfolio_server PRIVATE
  ${LIBURING_LIBRARIES}
  ${MYSQL_LIBS_LIST}
  Threads::Threads
)
