cmake_minimum_required(VERSION 3.20)

project(CallAgent VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

set(BUILD_SHARED OFF CACHE BOOL "Build libhv shared library" FORCE)
set(BUILD_STATIC ON CACHE BOOL "Build libhv static library" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Build libhv examples" FORCE)
set(BUILD_UNITTEST OFF CACHE BOOL "Build libhv tests" FORCE)

FetchContent_Declare(
    libhv
    GIT_REPOSITORY https://github.com/ithewei/libhv.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libhv)

add_executable(callagent
    src/main.cpp
)

target_link_libraries(callagent PRIVATE hv_static)

if(MSVC)
    target_compile_options(callagent PRIVATE /W4 /permissive-)
else()
    target_compile_options(callagent PRIVATE -Wall -Wextra -Wpedantic)
endif()
