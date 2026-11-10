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
    src/AssetManifest.cpp
    src/AudioCues.cpp
    src/Backlog.cpp
    src/Banter.cpp
    src/Battle.cpp
    src/BattleCamera.cpp
    src/Calibration.cpp
    src/ComboGenerator.cpp
    src/ActionButton.cpp
    src/ComboSequence.cpp
    src/Conductor.cpp
    src/Cutscene.cpp
    src/DefenceTimeline.cpp
    src/WeakPoint.cpp
    src/Dialogue.cpp
    src/DialogueLoop.cpp
    src/Emotion.cpp
    src/Encounter.cpp
    src/Explore.cpp
    src/FieldGoals.cpp
    src/WorldLayout.cpp
    src/Crowd.cpp
    src/CrowdGraph.cpp
    src/CrowdTuning.cpp
    src/Terrain.cpp
    src/MassingPlan.cpp
    src/MassingProps.cpp
    src/MassingDress.cpp
    src/MassingLayout.cpp
    src/WindowInterior.cpp
    src/WindowFacade.cpp
    src/FrameStats.cpp
    src/ShopFront.cpp
    src/WorldClock.cpp
    src/WorldChecks.cpp
    src/Roads.cpp
    src/RoadGraph.cpp
    src/CityPlacer.cpp
    src/OuterPlaces.cpp
    src/SurfaceTable.cpp
    src/HairCardSpec.cpp
    src/Overworld.cpp
    src/Face.cpp
    src/FaceDirection.cpp
    src/FaceImport.cpp
    src/GameplayPlan.cpp
    src/Guard.cpp
    src/Glb.cpp
    src/Json.cpp
    src/Loudness.cpp
    src/FightAnimator.cpp
    src/Traversal.cpp
    src/Slide.cpp
    src/MoveTuning.cpp
    src/DodgeRoll.cpp
    src/VehicleDrive.cpp
    src/VehicleRules.cpp
    src/LedgeHang.cpp
    src/Locomotion.cpp
    src/Mini.cpp
    src/UiText.cpp
    src/PartyTrail.cpp
    src/PathFollow.cpp
    src/MusicLoop.cpp
    src/Physics.cpp
    src/Progression.cpp
    src/Qte.cpp
    src/ReadCards.cpp
    src/Seen.cpp
    src/Settings.cpp
    src/SkinDetail.cpp
    src/Skip.cpp
    src/StoryDatabase.cpp
    src/StoryImport.cpp
    src/StoryRunner.cpp
    src/StubScreen.cpp
    src/SceneLook.cpp
    src/LookGrade.cpp
    src/LookTable.cpp
    src/SeaWaves.cpp
    src/StylePack.cpp
    src/StorySave.cpp
    src/TurnQueue.cpp
    src/Version.cpp
    src/Vfx.cpp
    src/Wav.cpp
    src/Inventory.cpp
    src/Shop.cpp
    src/ArenaFight.cpp
    src/FieldStrike.cpp
    src/ArenaZone.cpp
    src/ArrowLane.cpp
    src/StickFlick.cpp
    src/BeatFight.cpp
    src/BeatAccess.cpp
    src/BeatBoss.cpp
    src/ArenaBoss.cpp
    src/ArenaKindCrit.cpp
    src/StoneBearer.cpp
    src/BeatChoreo.cpp
    src/BeatMusic.cpp
    src/BeatMusicSynth.cpp
    src/BeatScreen.cpp
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

# ftcore_place: the city building placer's command-line tool (an offline build step, not the game).
add_executable(ftcore_place tools/ftcore_place.cpp)
target_link_libraries(ftcore_place PRIVATE ftcore ftcore_warnings)
if(MSVC)
    target_compile_options(ftcore_place PRIVATE /EHs-c- /GR-)
    target_compile_definitions(ftcore_place PRIVATE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(ftcore_place PRIVATE -fno-exceptions -fno-rtti)
endif()

# ftcore_layout: the lane layout's command-line tool for the private box tables (an offline build step, not the game).
add_executable(ftcore_layout tools/ftcore_layout.cpp)
target_link_libraries(ftcore_layout PRIVATE ftcore ftcore_warnings)
if(MSVC)
    target_compile_options(ftcore_layout PRIVATE /EHs-c- /GR-)
    target_compile_definitions(ftcore_layout PRIVATE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(ftcore_layout PRIVATE -fno-exceptions -fno-rtti)
endif()

# ftcore_roadgraph: the road graph's command-line tool for the private street and road tables (an offline build step).
add_executable(ftcore_roadgraph tools/ftcore_roadgraph.cpp)
target_link_libraries(ftcore_roadgraph PRIVATE ftcore ftcore_warnings)
if(MSVC)
    target_compile_options(ftcore_roadgraph PRIVATE /EHs-c- /GR-)
    target_compile_definitions(ftcore_roadgraph PRIVATE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(ftcore_roadgraph PRIVATE -fno-exceptions -fno-rtti)
endif()

# ftcore_frontcount: counts what a frontage table places on a map's boxes (an offline budget estimate).
add_executable(ftcore_frontcount tools/ftcore_frontcount.cpp)
target_link_libraries(ftcore_frontcount PRIVATE ftcore ftcore_warnings)
if(MSVC)
    target_compile_options(ftcore_frontcount PRIVATE /EHs-c- /GR-)
    target_compile_definitions(ftcore_frontcount PRIVATE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(ftcore_frontcount PRIVATE -fno-exceptions -fno-rtti)
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
        tests/test_asset_manifest.cpp
        tests/test_audio.cpp
        tests/test_backlog.cpp
        tests/test_banter.cpp
        tests/test_battle.cpp
        tests/test_battle_camera.cpp
        tests/test_turns_verbs.cpp
        tests/test_read_cards.cpp
        tests/test_turns_demo.cpp
        tests/test_load.cpp
        tests/test_battle_demo.cpp
        tests/test_combo.cpp
        tests/test_conductor.cpp
        tests/test_cutscene.cpp
        tests/test_defence.cpp
        tests/test_weakpoint.cpp
        tests/test_dialogue.cpp
        tests/test_dialogue_loop.cpp
        tests/test_alpha.cpp
        tests/test_emotion.cpp
        tests/test_encounter.cpp
        tests/test_explore.cpp
        tests/test_field_goals.cpp
        tests/test_world_layout.cpp
        tests/test_terrain.cpp
        tests/test_massing_plan.cpp
        tests/test_massing_props.cpp
        tests/test_massing_dress.cpp
        tests/test_massing_layout.cpp
        tests/test_window_interior.cpp
        tests/test_window_facade.cpp
        tests/test_frame_stats.cpp
        tests/test_shop_front.cpp
        tests/test_world_clock.cpp
        tests/test_world_checks.cpp
        tests/test_roads.cpp
        tests/test_road_graph.cpp
        tests/test_road_net.cpp
        tests/test_city_placer.cpp
        tests/test_outer_places.cpp
        tests/test_surface_table.cpp
        tests/test_hair_card_spec.cpp
        tests/test_overworld.cpp
        tests/test_face.cpp
        tests/test_face_direction.cpp
        tests/test_face_import.cpp
        tests/test_gameplay_plan.cpp
        tests/test_glb.cpp
        tests/test_json.cpp
        tests/test_fight_animator.cpp
        tests/test_traversal.cpp
        tests/test_slide.cpp
        tests/test_move_tuning.cpp
    tests/test_dodge_roll.cpp
    tests/test_vehicle_drive.cpp
    tests/test_vehicle_rules.cpp
    tests/test_ledge_hang.cpp
        tests/test_locomotion.cpp
        tests/test_mini.cpp
        tests/test_music_loop.cpp
        tests/test_party_trail.cpp
        tests/test_party_shore.cpp
        tests/test_crowd.cpp
        tests/test_guard.cpp
        tests/test_path_follow.cpp
        tests/test_ui_text.cpp
        tests/test_physics.cpp
        tests/test_progression.cpp
        tests/test_qte.cpp
        tests/test_settings.cpp
        tests/test_skin_detail.cpp
        tests/test_skip.cpp
        tests/test_story.cpp
        tests/test_story_chain.cpp
        tests/test_story_import.cpp
        tests/test_scene_look.cpp
        tests/test_look_grade.cpp
        tests/test_look_table.cpp
        tests/test_sea_waves.cpp
        tests/test_style.cpp
        tests/test_stub_screen.cpp
        tests/test_title.cpp
        tests/test_turnqueue.cpp
        tests/test_version.cpp
        tests/test_vfx.cpp
        tests/test_inventory.cpp
        tests/test_battle_items.cpp
        tests/test_shop.cpp
        tests/test_pause.cpp
        tests/test_arena.cpp
        tests/test_field_strike.cpp
        tests/test_arena_zone.cpp
        tests/test_arrow_lane.cpp
        tests/test_stick_flick.cpp
        tests/test_beat.cpp
        tests/test_beat_screen.cpp
        tests/test_beat_access.cpp
        tests/test_beat_boss.cpp
        tests/test_arena_boss.cpp
        tests/test_arena_kind_crit.cpp
        tests/test_stone_bearer.cpp
        tests/test_beat_choreo.cpp
        tests/test_beat_cine.cpp
        tests/test_beat_music.cpp
    )
    target_link_libraries(ftcore_tests PRIVATE ftcore doctest::doctest ftcore_warnings)
    # The real-data tests read the newengine checkout's assets in place (skipped when absent).
    set(FTCORE_NEWENGINE_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/../../newengine/assets" CACHE PATH
        "newengine assets folder (story script, face presets, golden data) for the real-data tests")
    target_compile_definitions(ftcore_tests PRIVATE FTCORE_TEST_ASSET_DIR="${FTCORE_NEWENGINE_ASSETS}"
        FTCORE_PROJECT_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../Data"
        FTCORE_CORE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    if(MSVC)
        target_compile_options(ftcore_tests PRIVATE /EHsc)
    endif()
    add_test(NAME ftcore_tests COMMAND ftcore_tests)

    # The placer's example job, end to end; its outputs stay in the build folder.
    add_test(NAME place_example COMMAND ftcore_place
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/place_example/job.json
        --placed ${CMAKE_CURRENT_BINARY_DIR}/place_example_placed.csv
        --summary ${CMAKE_CURRENT_BINARY_DIR}/place_example_summary.txt)
    set_tests_properties(place_example PROPERTIES PASS_REGULAR_EXPRESSION "PLACE OK")

    add_test(NAME unreal_wrappers COMMAND ${CMAKE_COMMAND}
        -DCORE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DWRAPPER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../Source/ForgottenCore/Private/Core
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckUnrealWrappers.cmake)
    add_test(NAME macro_names COMMAND ${CMAKE_COMMAND}
        -DINCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckMacroNames.cmake)
    add_test(NAME combo_fence COMMAND ${CMAKE_COMMAND}
        -DCORE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckComboFence.cmake)
    add_test(NAME ui_text_keys COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../Source/ForgottenThird
        -DTABLE=${CMAKE_CURRENT_SOURCE_DIR}/../Data/Loc/en/ui.json
        -DITEMS=${CMAKE_CURRENT_SOURCE_DIR}/../Data/Battle/items.json
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckUiTextKeys.cmake)
endif()
