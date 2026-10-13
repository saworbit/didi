# The id pairs separately copied server and extension binaries. The commit is
# read at build time; the stamp distinguishes configurations of the same source.
find_package(Git QUIET)
string(TIMESTAMP DIDI_BUILD_STAMP "%Y%m%dT%H%M%S" UTC)
set(DIDI_IDENTITY_GIT "")
if(GIT_FOUND)
    set(DIDI_IDENTITY_GIT "${GIT_EXECUTABLE}")
endif()

# Checking HEAD is cheap, and using git rather than a guessed ref dependency
# covers packed refs, detached HEAD, and worktrees whose .git is a pointer file.
# configure_file in the script preserves the output timestamp on a no-op, so
# only a changed id recompiles this one translation unit (#633, #1373).
add_custom_target(didi_build_identity
    COMMAND "${CMAKE_COMMAND}"
        "-DDIDI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DDIDI_OUTPUT=${CMAKE_BINARY_DIR}/generated/src/didi_build_id.cpp"
        "-DDIDI_GIT_EXECUTABLE=${DIDI_IDENTITY_GIT}"
        "-DDIDI_VERSION=${PROJECT_VERSION}"
        "-DDIDI_BUILD_STAMP=${DIDI_BUILD_STAMP}"
        -P "${CMAKE_CURRENT_LIST_DIR}/generate_build_id.cmake"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/generated/src/didi_build_id.cpp"
    VERBATIM
)
