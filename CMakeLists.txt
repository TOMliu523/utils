cmake_minimum_required(VERSION 3.26.5)

project(utils C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

add_executable(test_ring_spmc
    test/test_ring_spmc.c
)

add_executable(test_ring_mpsc
    test/test_ring_mpsc.c
)

target_link_libraries(test_ring utils)
target_link_libraries(test_ring pthread)

target_link_libraries(test_ring_spmc utils)
target_link_libraries(test_ring_spmc pthread)

target_link_libraries(test_ring_mpsc utils)
target_link_libraries(test_ring_mpsc pthread)
