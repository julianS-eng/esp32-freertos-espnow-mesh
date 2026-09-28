# Host build: unit tests (Unity) for the portable firmware modules, the golden
# vector generator used by the Python conformance tests, and the discrete-event
# mesh simulator. Nothing here depends on ESP-IDF.
#
#   cmake -S host -B host/build -G Ninja && cmake --build host/build && ctest --test-dir host/build
cmake_minimum_required(VERSION 3.16)
project(mesh_host LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)          # __attribute__((packed)) is a GNU extension
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MESH_HOST_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" ON)
option(MESH_HOST_COVERAGE "Build with gcov instrumentation" OFF)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(WARN_FLAGS -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Werror)

if(MESH_HOST_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()
if(MESH_HOST_COVERAGE)
    add_compile_options(--coverage -O0)
    add_link_options(--coverage)
endif()

# --- Unity (pinned to the v2.6.1 release commit) -----------------------------
include(FetchContent)
FetchContent_Declare(unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG cbcd08fa7de711053a3deec6339ee89cad5d2697 # v2.6.1
    GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(unity)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE UNITY_SUPPORT_64)

# --- Firmware modules compiled for the host ---------------------------------
set(PROTO_DIR ${REPO_ROOT}/components/mesh_protocol)
add_library(mesh_protocol STATIC
    ${PROTO_DIR}/mesh_crc.c
    ${PROTO_DIR}/mesh_protocol.c
    ${PROTO_DIR}/mesh_seq.c
    ${PROTO_DIR}/mesh_tx.c)
target_include_directories(mesh_protocol PUBLIC ${PROTO_DIR}/include)
target_compile_options(mesh_protocol PRIVATE ${WARN_FLAGS})

# --- Tests -------------------------------------------------------------------
enable_testing()
function(mesh_add_test name)
    add_executable(${name} test/${name}.c)
    target_link_libraries(${name} PRIVATE unity ${ARGN})
    target_compile_options(${name} PRIVATE ${WARN_FLAGS} -Wno-missing-prototypes)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

mesh_add_test(test_crc mesh_protocol)
mesh_add_test(test_codec mesh_protocol)
mesh_add_test(test_seq mesh_protocol)
mesh_add_test(test_tx mesh_protocol)
