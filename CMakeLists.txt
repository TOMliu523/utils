cmake_minimum_required(VERSION 3.26.5)

project(utils C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_FLAGS "-ggdb -fsanitize=address")
#set(CMAKE_C_FLAGS "-Ofast")

add_library(utils
    ring/ring.c
)

target_include_directories(utils
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/ring
)

add_executable(test_ring
    test/test_ring.c
)

target_link_libraries(test_ring utils)
target_link_libraries(test_ring pthread)
