# Fetch the Git Hash.
# Clear any inherited GIT_CEILING_DIRECTORIES so git can find the enclosing
# repo, which is needed when building under ESPHome.
execute_process(
    COMMAND
        ${CMAKE_COMMAND} -E env "GIT_CEILING_DIRECTORIES="
        git log -1 --format=%h
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# Fetch the Git Author Date with seconds
execute_process(
    COMMAND
        ${CMAKE_COMMAND} -E env "GIT_CEILING_DIRECTORIES="
        git log -1 --format=%ad --date=format:%Y-%m-%d\ %H:%M:%S
    OUTPUT_VARIABLE GIT_DATE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# Check for tracked file changes (-uno)
execute_process(
    COMMAND
        ${CMAKE_COMMAND} -E env "GIT_CEILING_DIRECTORIES="
        git status --porcelain -uno
    OUTPUT_VARIABLE GIT_STATUS_OUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if("${GIT_STATUS_OUT}" STREQUAL "")
    set(GIT_STATUS "clean")
else()
    set(GIT_STATUS "dirty")
endif()

# Fallbacks if git command fails or is missing
if(NOT GIT_HASH)
    set(GIT_HASH "hash unknown")
    set(GIT_DATE "date unknown")
    set(GIT_STATUS "")
endif()

