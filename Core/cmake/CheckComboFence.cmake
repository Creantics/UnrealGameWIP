# Run as: cmake -DCORE_DIR=<Core> -P CheckComboFence.cmake
# The skill arrow combos (ComboSequence, ComboGenerator) belong to the Turns battle alone
# (docs/COMBAT_SYSTEMS.md). Fails if an Arena or Beat source names them. The arrows themselves
# (Arrow.h) are the Turns battle's too: the Beat sources and the quick-time event, which answer with
# the action buttons (ActionButton.h), must not name Arrow, kArrows or arrowName. Line comments are
# ignored.

set(files
    include/ftcore/ArenaFight.h src/ArenaFight.cpp
    include/ftcore/BeatFight.h src/BeatFight.cpp
    include/ftcore/BeatScreen.h src/BeatScreen.cpp)
file(GLOB more RELATIVE "${CORE_DIR}" "${CORE_DIR}/include/ftcore/BeatBoss*.h" "${CORE_DIR}/src/BeatBoss*.cpp"
    "${CORE_DIR}/include/ftcore/ArenaLoad*.h" "${CORE_DIR}/src/ArenaLoad*.cpp")
list(APPEND files ${more})
set(pattern "(ComboSequence|ComboGenerator|generateCombo)")
set(problems "")
foreach(rel IN LISTS files)
    set(path "${CORE_DIR}/${rel}")
    if(NOT EXISTS "${path}")
        string(APPEND problems "  ${rel}: missing\n")
        continue()
    endif()
    file(STRINGS "${path}" lines)
    set(number 0)
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        string(REGEX REPLACE "//.*$" "" code "${line}")
        if(code MATCHES "${pattern}")
            string(APPEND problems "  ${rel}:${number}: '${CMAKE_MATCH_1}'\n")
        endif()
    endforeach()
endforeach()

set(buttonFiles include/ftcore/Qte.h src/Qte.cpp)
foreach(rel IN LISTS files)
    if(rel MATCHES "Beat")
        list(APPEND buttonFiles "${rel}")
    endif()
endforeach()
set(arrowPattern "(^|[^A-Za-z0-9_])(Arrow|kArrows|arrowName)([^A-Za-z0-9_]|$)")
foreach(rel IN LISTS buttonFiles)
    set(path "${CORE_DIR}/${rel}")
    if(NOT EXISTS "${path}")
        string(APPEND problems "  ${rel}: missing\n")
        continue()
    endif()
    file(STRINGS "${path}" lines)
    set(number 0)
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        string(REGEX REPLACE "//.*$" "" code "${line}")
        if(code MATCHES "${arrowPattern}")
            string(APPEND problems "  ${rel}:${number}: '${CMAKE_MATCH_2}' (answer with ActionButton)\n")
        endif()
    endforeach()
endforeach()

if(problems)
    message(FATAL_ERROR "Arena, Beat or QTE sources name the Turns battle's arrows or arrow combos:\n${problems}")
endif()
list(LENGTH files count)
list(LENGTH buttonFiles buttonCount)
message(STATUS "No arrow combos in ${count} Arena and Beat sources, no arrows in ${buttonCount} Beat and QTE sources")
