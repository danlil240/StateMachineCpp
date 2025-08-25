cmake_minimum_required(VERSION 3.16)
project(StateMachineModular VERSION 1.0.0 LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler settings
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Header-only library (since we converted to templates)
add_library(StateMachineLib INTERFACE)

# Set C++17 standard for the library
target_compile_features(StateMachineLib INTERFACE cxx_std_17)

target_include_directories(StateMachineLib
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Examples
add_executable(simple_example examples/simple_example.cpp)
target_link_libraries(simple_example StateMachineLib)

add_executable(example examples/example.cpp)
target_link_libraries(example StateMachineLib)  

# Tests
enable_testing()
add_executable(sm_test tests/test.cpp)
target_link_libraries(sm_test StateMachineLib)
add_test(NAME sm_test COMMAND sm_test)

# Installation
install(TARGETS StateMachineLib
    EXPORT StateMachineLibTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)

install(EXPORT StateMachineLibTargets
    FILE StateMachineLibConfig.cmake
    DESTINATION lib/cmake/StateMachineLib
)
