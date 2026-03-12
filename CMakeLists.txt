cmake_minimum_required(VERSION 3.25)
project(async_worker LANGUAGES CXX)

# ── Compiler settings ─────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # strict ISO C++, no GNU extensions

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── Library target ────────────────────────────────────────────────────────────
add_library(async_worker_lib STATIC async_worker.cpp)

target_include_directories(async_worker_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(async_worker_lib PUBLIC Threads::Threads)

target_compile_options(async_worker_lib PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
)

# ── Demo executable ───────────────────────────────────────────────────────────
add_executable(async_worker_demo main.cpp)

target_link_libraries(async_worker_demo PRIVATE async_worker_lib)

target_compile_options(async_worker_demo PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
)

# ── Optional: enable AddressSanitizer in Debug builds ─────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(async_worker_lib    PRIVATE -fsanitize=address,undefined)
    target_compile_options(async_worker_demo   PRIVATE -fsanitize=address,undefined)
    target_link_options(async_worker_demo PRIVATE -fsanitize=address,undefined)
endif()
