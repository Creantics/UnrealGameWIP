cmake_minimum_required(VERSION 3.24)
project(ForgottenThirdCore LANGUAGES CXX)

# ftcore: game logic as plain C++20. No Unreal headers, no exceptions, no RTTI.
# Unreal compiles the same sources through Source/ForgottenCore (one wrapper .cpp per file here).

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    # CMake adds /EHsc to every target; exceptions are set per target below instead.
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

option(FTCORE_BUILD_TESTS "Build ftcore unit tests" ON)
option(FTCORE_WERROR "Treat warnings in ftcore and its tests as errors" OFF)

add_library(ftcore_warnings INTERFACE)
if(MSVC)
    target_compile_options(ftcore_warnings INTERFACE /W4 /permissive- /utf-8)
    if(FTCORE_WERROR)
        target_compile_options(ftcore_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(ftcore_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    if(FTCORE_WERROR)
        target_compile_options(ftcore_warnings INTERFACE -Werror)
    endif()
endif()

add_library(ftcore STATIC
    src/Battle.cpp
    src/Calibration.cpp
    src/ComboGenerator.cpp
    src/ComboSequence.cpp
    src/Conductor.cpp
    src/StoryDatabase.cpp
    src/StoryRunner.cpp
    src/StorySave.cpp
    src/TurnQueue.cpp
    src/Version.cpp
)
target_include_directories(ftcore PUBLIC include)
target_link_libraries(ftcore PRIVATE ftcore_warnings)
# Only the library itself: doctest in the tests needs exceptions.
if(MSVC)
    target_compile_options(ftcore PRIVATE /EHs-c- /GR-)
    target_compile_definitions(ftcore PRIVATE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(ftcore PRIVATE -fno-exceptions -fno-rtti)
endif()

if(FTCORE_BUILD_TESTS)
    include(FetchContent)
    # CMake 4 rejects cmake_minimum_required(< 3.5), which doctest 2.4.11 still declares.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(doctest)

    enable_testing()
    add_executable(ftcore_tests
        tests/test_main.cpp
        tests/AllocCounter.cpp
        tests/AllocHook.cpp
        tests/test_battle.cpp
        tests/test_combo.cpp
        tests/test_conductor.cpp
        tests/test_story.cpp
        tests/test_turnqueue.cpp
        tests/test_version.cpp
    )
    target_link_libraries(ftcore_tests PRIVATE ftcore doctest::doctest ftcore_warnings)
    if(MSVC)
        target_compile_options(ftcore_tests PRIVATE /EHsc)
    endif()
    add_test(NAME ftcore_tests COMMAND ftcore_tests)

    add_test(NAME unreal_wrappers COMMAND ${CMAKE_COMMAND}
        -DCORE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DWRAPPER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../Source/ForgottenCore/Private/Core
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckUnrealWrappers.cmake)
    add_test(NAME macro_names COMMAND ${CMAKE_COMMAND}
        -DINCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckMacroNames.cmake)
endif()
