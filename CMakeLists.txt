cmake_minimum_required(VERSION 3.20)

project(CallAgent VERSION 0.2.0 LANGUAGES C CXX)

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

if(WIN32)
    message(FATAL_ERROR "This runtime currently supports POSIX; Windows native transports are not implemented")
endif()
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(ICU REQUIRED COMPONENTS uc i18n)
find_package(Threads REQUIRED)
file(READ resources/claude-desktop/hooks/register.js CALLAGENT_CLAUDE_SCRIPT)
string(SHA256 CALLAGENT_BUNDLE_ID "${CALLAGENT_CLAUDE_SCRIPT}")
string(REPLACE "__CALLAGENT_BUNDLE__" "${CALLAGENT_BUNDLE_ID}" CALLAGENT_CLAUDE_SCRIPT "${CALLAGENT_CLAUDE_SCRIPT}")
configure_file(resources/ClaudeBundle.hpp.in generated/ClaudeBundle.hpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS resources/claude-desktop/hooks/register.js)

add_library(callagent_core
    src/domain/Session.cpp
    src/application/Config.cpp
    src/application/Service.cpp
    src/application/WorkBuddySupervisor.cpp
    src/persistence/Files.cpp
    src/persistence/WorkBuddyRepository.cpp
    src/providers/Codex.cpp
    src/providers/Claude.cpp
    src/providers/WorkBuddy.cpp
    src/skills/Publisher.cpp
    src/transport/NativeChannel.cpp
    src/transport/WorkBuddyRpc.cpp
    src/transport/Daemon.cpp
)
target_include_directories(callagent_core PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(callagent_core PUBLIC hv_static PRIVATE OpenSSL::Crypto SQLite::SQLite3 ICU::uc ICU::i18n Threads::Threads)
target_compile_options(callagent_core PRIVATE -Wall -Wextra -Wpedantic)
add_executable(callagent src/main.cpp)
target_link_libraries(callagent PRIVATE callagent_core)
target_compile_options(callagent PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS callagent RUNTIME DESTINATION bin)
include(CTest)
if(BUILD_TESTING)
    add_executable(callagent_tests tests/core.cpp)
    target_link_libraries(callagent_tests PRIVATE callagent_core)
    add_test(NAME callagent_core COMMAND callagent_tests)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    if(APPLE)
        add_executable(workbuddy_desktop_fixture tests/workbuddy_desktop_fixture.cpp)
        target_link_libraries(workbuddy_desktop_fixture PRIVATE hv_static Threads::Threads)
        add_test(NAME workbuddy_supervisor COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/workbuddy_supervisor.py" $<TARGET_FILE:callagent> $<TARGET_FILE:workbuddy_desktop_fixture>)
        set_tests_properties(workbuddy_supervisor PROPERTIES TIMEOUT 120)
        add_test(NAME desktop_protocols COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py" $<TARGET_FILE:callagent>)
        set_tests_properties(desktop_protocols PROPERTIES TIMEOUT 120)
    endif()
    find_program(CALLAGENT_NODE node)
    if(CALLAGENT_NODE)
        add_test(NAME claude_plugin COMMAND "${CALLAGENT_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/claude_plugin.mjs" "${CMAKE_CURRENT_SOURCE_DIR}/resources/claude-desktop/hooks/register.js")
    endif()
endif()
