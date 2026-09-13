cmake_minimum_required(VERSION 3.16)

project(embedded_comm_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(protocol
    protocol/codec.cpp
    protocol/crc32.cpp
)

target_include_directories(protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(tcp_sender
    tcp/sender.cpp
)

target_link_libraries(tcp_sender
    protocol
)

add_executable(tcp_receiver
    tcp/receiver.cpp
)

target_link_libraries(tcp_receiver
    protocol
)
