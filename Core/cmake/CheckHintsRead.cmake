# Run as: cmake -DSOURCE_DIR=<Source/ForgottenThird> -DHINTS=<Core/src/InputBindings.cpp> -P CheckHintsRead.cmake
# Every action a controls line hints (the kArenaHint and kFieldHint tables, "A::<Action>") must be read by the host:
# named as ftcore::InputAction::<Action> in code (comments stripped) somewhere in the game module. A hint for an
# action nothing reads would send the player to a dead key.

file(READ "${HINTS}" hints_text)
string(REGEX MATCH "constexpr InputHintEntry kArenaHint.*kFieldHint[^;]*;" tables "${hints_text}")
if(tables STREQUAL "")
    message(FATAL_ERROR "hints_read: no kArenaHint/kFieldHint tables found in ${HINTS}")
endif()
string(REGEX MATCHALL "A::[A-Za-z0-9]+" hinted "${tables}")
list(TRANSFORM hinted REPLACE "^A::" "")
list(REMOVE_DUPLICATES hinted)

file(GLOB sources "${SOURCE_DIR}/*.cpp" "${SOURCE_DIR}/*.h")
set(read "")
foreach(path IN LISTS sources)
    file(STRINGS "${path}" lines)
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "//.*$" "" code "${line}")
        string(REGEX MATCHALL "InputAction::[A-Za-z0-9]+" hits "${code}")
        foreach(hit IN LISTS hits)
            string(REPLACE "InputAction::" "" name "${hit}")
            list(APPEND read "${name}")
        endforeach()
    endforeach()
endforeach()

set(problems "")
foreach(action IN LISTS hinted)
    if(NOT action IN_LIST read)
        string(APPEND problems "  hinted but never read by the host: ${action}
")
    endif()
endforeach()
if(problems)
    message(FATAL_ERROR "hints_read:
${problems}")
endif()
list(LENGTH hinted n)
message(STATUS "hints_read: ${n} hinted actions, each read by the host")
