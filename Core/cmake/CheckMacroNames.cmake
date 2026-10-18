# Run as: cmake -DINCLUDE_DIR=<Core/include> -P CheckMacroNames.cmake
# Core's public headers are also compiled inside Unreal modules, after Unreal's own headers, and
# on Linux after glibc's. Fails if a header uses an identifier one of those defines as a macro.
# Line comments are ignored.

set(reserved check checkf checkSlow verify verifyf ensure ensureMsgf PI TEXT major minor)
list(JOIN reserved "|" alternatives)
set(pattern "(^|[^A-Za-z0-9_])(${alternatives})([^A-Za-z0-9_]|$)")

file(GLOB_RECURSE headers "${INCLUDE_DIR}/*.h")
set(problems "")
foreach(header IN LISTS headers)
    file(STRINGS "${header}" lines)
    set(number 0)
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        string(REGEX REPLACE "//.*$" "" code "${line}")
        if(code MATCHES "${pattern}")
            string(APPEND problems "  ${header}:${number}: '${CMAKE_MATCH_2}'\n")
        endif()
    endforeach()
endforeach()

if(problems)
    message(FATAL_ERROR "Core headers use identifiers that Unreal or glibc define as macros:\n${problems}")
endif()
list(LENGTH headers count)
message(STATUS "No reserved macro names in ${count} Core headers")
