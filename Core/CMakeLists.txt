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
    src/Dialogue.cpp
    src/Emotion.cpp
    src/Encounter.cpp
    src/Explore.cpp
    src/Face.cpp
    src/FaceDirection.cpp
    src/FaceImport.cpp
    src/GameplayPlan.cpp
    src/Json.cpp
    src/Physics.cpp
    src/StoryDatabase.cpp
    src/StoryImport.cpp
    src/StoryRunner.cpp
    src/StubScreen.cpp
    src/StylePack.cpp
    src/StorySave.cpp
    src/TurnQueue.cpp
    src/Version.cpp
    src/Vfx.cpp
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
        tests/test_battle_demo.cpp
        tests/test_combo.cpp
        tests/test_conductor.cpp
        tests/test_dialogue.cpp
        tests/test_alpha.cpp
        tests/test_emotion.cpp
        tests/test_encounter.cpp
        tests/test_explore.cpp
        tests/test_face.cpp
        tests/test_face_direction.cpp
        tests/test_face_import.cpp
        tests/test_gameplay_plan.cpp
        tests/test_json.cpp
        tests/test_physics.cpp
        tests/test_story.cpp
        tests/test_story_chain.cpp
        tests/test_story_import.cpp
        tests/test_style.cpp
        tests/test_stub_screen.cpp
        tests/test_title.cpp
        tests/test_turnqueue.cpp
        tests/test_version.cpp
        tests/test_vfx.cpp
    )
    target_link_libraries(ftcore_tests PRIVATE ftcore doctest::doctest ftcore_warnings)
    # The real-data tests read the newengine checkout's assets in place (skipped when absent).
    set(FTCORE_NEWENGINE_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/../../newengine/assets" CACHE PATH
        "newengine assets folder (story script, face presets, golden data) for the real-data tests")
    target_compile_definitions(ftcore_tests PRIVATE FTCORE_TEST_ASSET_DIR="${FTCORE_NEWENGINE_ASSETS}"
        FTCORE_PROJECT_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../Data")
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
