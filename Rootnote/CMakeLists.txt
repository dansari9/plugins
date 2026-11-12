cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal macOS binary")
endif()

# Bump on every build that is handed out: Logic caches Audio Units by version.
project(Rootnote VERSION 0.4.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ROOTNOTE_BUILD_PLUGIN "Build the plug-in. OFF builds only the JUCE-free core and its tests" ON)
option(ROOTNOTE_BUILD_TESTS "Build the unit tests and the benchmark" ON)
option(ROOTNOTE_COPY_AFTER_BUILD "Copy plugin binaries into the user plug-in folders" OFF)
option(ROOTNOTE_SANITIZE "Build the core and its tests with address and undefined-behaviour sanitizers" OFF)

if(ROOTNOTE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

function(rootnote_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wdouble-promotion -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# RootnoteCore: all DSP and analysis. Plain C++17 with no JUCE, so it is
# compiled once and linked into the plug-in, the tests and the benchmark.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(RootnoteCore STATIC
    Source/dsp/BandBank.cpp
    Source/dsp/Engine.cpp
    Source/dsp/ModalRing.cpp
    Source/dsp/NoteMath.cpp
    Source/analysis/AnalysisWorker.cpp
    Source/analysis/OnsetDetector.cpp
    Source/analysis/PitchDetector.cpp
    Source/analysis/Tuner.cpp
)
target_include_directories(RootnoteCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Source)
target_link_libraries(RootnoteCore PUBLIC Threads::Threads)
rootnote_warnings(RootnoteCore)

if(ROOTNOTE_BUILD_TESTS)
    enable_testing()

    add_executable(RootnoteCoreTests
        Tests/CoreTestMain.cpp
        Tests/NoteMathTests.cpp
        Tests/FilterTests.cpp
        Tests/BandBankTests.cpp
        Tests/RingAndLimiterTests.cpp
        Tests/MidiTests.cpp
        Tests/EngineTests.cpp
        Tests/AnalysisTests.cpp
        Tests/TunerTests.cpp
    )
    target_link_libraries(RootnoteCoreTests PRIVATE RootnoteCore)
    rootnote_warnings(RootnoteCoreTests)
    set_target_properties(RootnoteCoreTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME rootnote_core COMMAND RootnoteCoreTests)

    add_executable(RootnoteBenchmark Tests/Benchmark.cpp)
    target_link_libraries(RootnoteBenchmark PRIVATE RootnoteCore)
    rootnote_warnings(RootnoteBenchmark)
    set_target_properties(RootnoteBenchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

if(NOT ROOTNOTE_BUILD_PLUGIN)
    return()
endif()

include(cmake/Plugin.cmake)
