cmake_minimum_required(VERSION 3.20)
project(hotpath VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g")

# Strict warnings apply to OUR code only. Applied globally they also hit
# FetchContent dependencies (Catch2), whose warnings then bury ours.
set(HOTPATH_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

# CI compiles with -Werror; local builds do not. A newer compiler inventing a
# new diagnostic should not stop work in progress, but a warning must not be
# able to accumulate unnoticed either -- and they had: ten of them, including a
# dropped [[nodiscard]] and two printf format mismatches that only one of the
# three front-ends could see. The gate is what makes "warning-clean" a fact
# rather than a habit.
option(HOTPATH_WERROR "Treat warnings as errors (used by CI)" OFF)
if(HOTPATH_WERROR)
  list(APPEND HOTPATH_WARNINGS -Werror)
endif()

# ---- sanitizer options (mutually exclusive builds) ----
option(HOTPATH_ASAN "Build with AddressSanitizer + UBSan" OFF)
option(HOTPATH_TSAN "Build with ThreadSanitizer" OFF)
if(HOTPATH_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(HOTPATH_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

# ---- the hot path is header-only; this is the interface target ----
add_library(hotpath INTERFACE)
target_include_directories(hotpath INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(hotpath INTERFACE ${HOTPATH_WARNINGS})

# ---- instrumentation lives in a separate lib so it is never linked into a
#      build we intend to measure unless explicitly asked for.
#
#      It MUST be SHARED. The syscall counter works by dyld __interpose, and
#      dyld only honours an __interpose section from a dylib loaded at launch --
#      from a static archive it is accepted and silently ignored, leaving the
#      counter reading zero forever. Verified by syscall_counting_active(). ----
add_library(hotpath_instrument SHARED src/alloc_counter.cpp)
if(NOT APPLE)
  # Linux reaches the real libc entry points through dlsym(RTLD_NEXT), and
  # sched_setaffinity/CPU_SET need the GNU extensions.
  target_link_libraries(hotpath_instrument PUBLIC ${CMAKE_DL_LIBS})
  target_compile_definitions(hotpath INTERFACE _GNU_SOURCE)
  target_compile_definitions(hotpath_instrument PRIVATE _GNU_SOURCE)
endif()
target_include_directories(hotpath_instrument PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(hotpath_instrument PRIVATE ${HOTPATH_WARNINGS})

add_subdirectory(src)
add_subdirectory(bench)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
