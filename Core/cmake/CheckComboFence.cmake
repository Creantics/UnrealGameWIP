# Run as: cmake -DCORE_DIR=<Core> -P CheckComboFence.cmake
# The skill arrow combos (ComboSequence, ComboGenerator) belong to the Turns battle alone
# (docs/COMBAT_SYSTEMS.md). Fails if an Arena or Beat source names them. Line comments are ignored.

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

if(problems)
    message(FATAL_ERROR "Arena and Beat sources name the Turns battle's arrow combos:\n${problems}")
endif()
list(LENGTH files count)
message(STATUS "No arrow combos in ${count} Arena and Beat sources")
