cmake_minimum_required(VERSION 3.22.1)

project("gaussiansplatting")

add_library(${CMAKE_PROJECT_NAME} SHARED
    renderer.cpp)

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(${CMAKE_PROJECT_NAME}
    ${android-lib}
    ${log-lib}
    vulkan)