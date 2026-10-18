# Run as: cmake -DCORE_DIR=<Core> -DWRAPPER_DIR=<Source/ForgottenCore/Private/Core> -P CheckUnrealWrappers.cmake
# Unreal compiles Core through one wrapper per Core/src/*.cpp. Fails if any source has no wrapper,
# a wrapper has no source, or a wrapper does anything but include its source.

file(GLOB sources RELATIVE "${CORE_DIR}/src" "${CORE_DIR}/src/*.cpp")
file(GLOB wrappers RELATIVE "${WRAPPER_DIR}" "${WRAPPER_DIR}/*.cpp")

set(problems "")
foreach(name IN LISTS sources)
    if(NOT name IN_LIST wrappers)
        string(APPEND problems "  missing wrapper: ${WRAPPER_DIR}/${name}\n")
    endif()
endforeach()
foreach(name IN LISTS wrappers)
    if(NOT name IN_LIST sources)
        string(APPEND problems "  wrapper without source: ${WRAPPER_DIR}/${name}\n")
        continue()
    endif()
    file(READ "${WRAPPER_DIR}/${name}" content)
    string(STRIP "${content}" content)
    if(NOT content STREQUAL "#include \"src/${name}\"")
        string(APPEND problems "  ${WRAPPER_DIR}/${name} must contain exactly: #include \"src/${name}\"\n")
    endif()
endforeach()

if(problems)
    message(FATAL_ERROR "Unreal wrappers out of sync with Core/src:\n${problems}")
endif()
list(LENGTH sources count)
message(STATUS "Unreal wrappers match ${count} Core sources")
