cmake_minimum_required(VERSION 3.20)

project(pfind VERSION 1.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PFIND_BUILD_TESTS "Build the unit tests" ON)
option(PFIND_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Everything except main() lives in a library so tests can link against it.
add_library(pfind_core STATIC
    src/CLIParser.cpp
    src/Completion.cpp
    src/Format.cpp
    src/Replace.cpp
    src/IgnoreRules.cpp
    src/SearchEngine.cpp
    src/Utf8.cpp
    src/Wildcard.cpp
)

find_package(Threads REQUIRED)
target_link_libraries(pfind_core PUBLIC Threads::Threads)

target_include_directories(pfind_core PUBLIC include PRIVATE src)
target_compile_features(pfind_core PUBLIC cxx_std_20)
target_compile_definitions(pfind_core PUBLIC PFIND_VERSION="${PROJECT_VERSION}")

add_executable(pfind src/main.cpp)
target_link_libraries(pfind PRIVATE pfind_core)

if(MSVC)
    set(PFIND_WARNING_FLAGS /W4 /permissive- /utf-8)
    if(PFIND_WARNINGS_AS_ERRORS)
        list(APPEND PFIND_WARNING_FLAGS /WX)
    endif()
else()
    set(PFIND_WARNING_FLAGS -Wall -Wextra -Wpedantic)
    if(PFIND_WARNINGS_AS_ERRORS)
        list(APPEND PFIND_WARNING_FLAGS -Werror)
    endif()
endif()

target_compile_options(pfind_core PRIVATE ${PFIND_WARNING_FLAGS})
target_compile_options(pfind PRIVATE ${PFIND_WARNING_FLAGS})

if(WIN32)
    target_compile_definitions(pfind_core PUBLIC NOMINMAX UNICODE _UNICODE)
    target_link_libraries(pfind PRIVATE shell32)
endif()

if(PFIND_BUILD_TESTS)
    enable_testing()
    add_executable(pfind_tests tests/tests.cpp)
    target_link_libraries(pfind_tests PRIVATE pfind_core)
    target_compile_options(pfind_tests PRIVATE ${PFIND_WARNING_FLAGS})
    add_test(NAME pfind_tests COMMAND pfind_tests)
endif()

include(GNUInstallDirs)
install(TARGETS pfind RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
