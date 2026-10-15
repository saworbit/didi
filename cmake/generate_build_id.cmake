set(DIDI_BUILD_COMMIT "nogit")
if(DIDI_GIT_EXECUTABLE)
    execute_process(
        COMMAND "${DIDI_GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${DIDI_SOURCE_DIR}"
        OUTPUT_VARIABLE DIDI_BUILD_COMMIT_RAW
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE DIDI_BUILD_COMMIT_STATUS
    )
    if(DIDI_BUILD_COMMIT_STATUS EQUAL 0 AND DIDI_BUILD_COMMIT_RAW)
        set(DIDI_BUILD_COMMIT "${DIDI_BUILD_COMMIT_RAW}")
        # The stamp is the later of the configure time and the commit time.
        # From the configure alone, two commits built in one build directory
        # carried one stamp and setup could not tell which was newer (#1450).
        # The configure time still wins for a commit older than the configure,
        # so a reconfigure still reads as a different build. string(TIMESTAMP)
        # formats SOURCE_DATE_EPOCH, which keeps git's time zone out of it.
        execute_process(
            COMMAND "${DIDI_GIT_EXECUTABLE}" show -s --format=%ct HEAD
            WORKING_DIRECTORY "${DIDI_SOURCE_DIR}"
            OUTPUT_VARIABLE DIDI_COMMIT_EPOCH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE DIDI_COMMIT_EPOCH_STATUS
        )
        if(DIDI_COMMIT_EPOCH_STATUS EQUAL 0 AND DIDI_COMMIT_EPOCH MATCHES "^[0-9]+$")
            set(ENV{SOURCE_DATE_EPOCH} "${DIDI_COMMIT_EPOCH}")
            string(TIMESTAMP DIDI_COMMIT_STAMP "%Y%m%dT%H%M%S" UTC)
            unset(ENV{SOURCE_DATE_EPOCH})
            if(DIDI_COMMIT_STAMP STRGREATER DIDI_BUILD_STAMP)
                set(DIDI_BUILD_STAMP "${DIDI_COMMIT_STAMP}")
            endif()
        endif()
    endif()
endif()
set(DIDI_BUILD_ID "${DIDI_VERSION}+${DIDI_BUILD_COMMIT}.${DIDI_BUILD_STAMP}")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/build_id.cpp.in"
    "${DIDI_OUTPUT}"
    @ONLY
)
