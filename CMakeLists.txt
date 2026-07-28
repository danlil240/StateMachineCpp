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

add_executable(non_chaining_example examples/non_chaining_example.cpp)
target_link_libraries(non_chaining_example StateMachineLib)

add_executable(regions_example examples/regions_example.cpp)
target_link_libraries(regions_example StateMachineLib)

add_executable(nested_example examples/nested_example.cpp)
target_link_libraries(nested_example StateMachineLib)

# Tests
enable_testing()

add_executable(sm_test tests/test.cpp)
target_link_libraries(sm_test StateMachineLib)
add_test(NAME sm_test COMMAND sm_test)

add_executable(sm_regions_test tests/state_machine_regions_test.cpp)
target_link_libraries(sm_regions_test StateMachineLib)
add_test(NAME sm_regions_test COMMAND sm_regions_test)

# Google Test based tests
find_package(GTest REQUIRED)
add_executable(sm_gtest tests/StateMachineTest.cpp)
target_link_libraries(sm_gtest StateMachineLib GTest::gtest GTest::gtest_main)
add_test(NAME sm_gtest COMMAND sm_gtest)

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
