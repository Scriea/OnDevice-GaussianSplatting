cmake_minimum_required(VERSION 3.22.1)

project("gaussiansplatting")

add_library(${CMAKE_PROJECT_NAME} SHARED
    renderer.cpp)

target_link_libraries(${CMAKE_PROJECT_NAME}
    android
    log
    vulkan)