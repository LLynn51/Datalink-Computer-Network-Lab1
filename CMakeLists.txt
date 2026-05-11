cmake_minimum_required(VERSION 3.10)

project(ComputerNetworkLab1 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

file(GLOB SOURCES "*.c")

add_executable(datalink_lab ${SOURCES})
