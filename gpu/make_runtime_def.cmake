# The export list of the runtime becomes the definition file of the import library the
# GPU library links against (the runtime's functions live in the program, not in the
# GPU library).
file(STRINGS "${RUNTIME_EXPORTS}" RUNTIME_EXPORT_LINES REGEX "^runtime_")
set(RUNTIME_DEF_CONTENT "EXPORTS\n")
foreach(symbol ${RUNTIME_EXPORT_LINES})
    string(APPEND RUNTIME_DEF_CONTENT "    ${symbol}\n")
endforeach()
file(WRITE "${RUNTIME_DEF}" "${RUNTIME_DEF_CONTENT}")
