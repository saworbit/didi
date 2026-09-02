cmake_minimum_required(VERSION 3.20)
project(didi VERSION 1.4.0 LANGUAGES C CXX)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DIDI_BUILD_TESTS "Build the didi test executables" ON)
option(DIDI_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(DIDI_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR
            "DIDI_ENABLE_SANITIZERS is for the GCC and Clang builds. MSVC needs "
            "/fsanitize=address on its own and has no UBSan, so it is not wired up here.")
    endif()
    # On the compile side and the link side both, or the runtime is missing.
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# Carried on didi_core as PUBLIC rather than set globally, so every target that
# links it inherits the same set and nothing picks these up by accident.
set(DIDI_INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/didi/common
    ${CMAKE_CURRENT_SOURCE_DIR}/include/didi/mcp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/didi/offline
    ${CMAKE_CURRENT_SOURCE_DIR}/include/didi/gdextension
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

set(PHASE7_SCHEMA_NAMES
    anim_list_tracks anim_play_track gridmap_set_cells nav_bake_mesh nav_query_path
    physics_raycast_query physics_simulate_step runtime_get_call_stack runtime_inject_input
    runtime_read_profiler signal_connect signal_disconnect signal_emit signal_list_connections
    tilemap_get_used_rect tilemap_set_cells viewport_set_camera_transform
    viewport_toggle_debug_draw
)
set(PHASE7_SCHEMA_FILES)
foreach(name IN LISTS PHASE7_SCHEMA_NAMES)
    list(APPEND PHASE7_SCHEMA_FILES "${CMAKE_CURRENT_SOURCE_DIR}/schemas/phase7/${name}.schema.json")
endforeach()
set(PHASE7_GENERATED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/didi/mcp/phase7_schemas.hpp")
set(PHASE7_GENERATED_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/didi/mcp/phase7_schemas.cpp")
add_custom_command(
    OUTPUT "${PHASE7_GENERATED_HEADER}" "${PHASE7_GENERATED_SOURCE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_phase7_schemas.py"
            --schema-dir "${CMAKE_CURRENT_SOURCE_DIR}/schemas/phase7"
            --header "${PHASE7_GENERATED_HEADER}"
            --source "${PHASE7_GENERATED_SOURCE}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_phase7_schemas.py" ${PHASE7_SCHEMA_FILES}
    VERBATIM
)

# Offline class reflection answers from Godot's own API surface rather than a
# hand written snapshot. resources/didi_class_reference.json is the trimmed form
# of a `godot --dump-extension-api` dump, committed because the dump itself is a
# local artifact that no build machine has. Refresh it with
# tools/generate_class_reference.py when the pinned Godot version moves.
#
# Read at runtime rather than compiled in: a megabyte and a half of generated
# C++ would be paid for on every build on every platform, and this changes only
# when the pinned version does.
set(DIDI_CLASS_REFERENCE "${CMAKE_CURRENT_SOURCE_DIR}/resources/didi_class_reference.json")
if(NOT EXISTS "${DIDI_CLASS_REFERENCE}")
    message(WARNING
        "resources/didi_class_reference.json is missing. script_reflect_class will "
        "fall back to its small built-in class map. Regenerate it with "
        "tools/generate_class_reference.py.")
    set(DIDI_CLASS_REFERENCE "")
endif()

if(MSVC)
    add_compile_options(/W3 /utf-8 /EHsc /MP /permissive- /wd4100 /wd4505)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DWIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Core sources
set(CORE_SOURCES
    src/common/logger.cpp
    src/common/base64.cpp
    src/common/png.cpp
    src/common/image_diff.cpp
    src/common/capture_cache.cpp
    src/common/secure_random.cpp
    src/common/ipc_channel_win32.cpp
    src/mcp/jsonrpc.cpp
    src/mcp/mcp_server.cpp
    src/mcp/tool_registry.cpp
    src/mcp/resource_registry.cpp
    src/mcp/prompt_registry.cpp
    src/runtime/session_client.cpp
    src/runtime/session_lock.cpp
    src/runtime/profiler_collector.cpp
    src/runtime/input_injection.cpp
    src/offline/class_reference.cpp
    src/mcp/mutation_safety.cpp
    src/tools/phase7_live_forward.cpp
    src/tools/hierarchy_view.cpp
    ${PHASE7_GENERATED_SOURCE}
    src/gdextension/session_host.cpp
    src/gdextension/runtime_log.cpp
    src/gdextension/engine_output_logger.cpp
    src/gdextension/runtime_request_router.cpp
    src/gdextension/expression_sandbox.cpp
    src/offline/gdscript_diagnostics.cpp
    src/offline/resource_indexer.cpp
    src/offline/audio_bus_layout.cpp
    src/offline/project_audit.cpp
    src/offline/project_impact.cpp
    src/offline/project_text_scan.cpp
    src/offline/project_search.cpp
    src/offline/test_runner.cpp
    src/offline/process_runner.cpp
    src/offline/deep_domain_support.cpp
    src/tools/visual_tools.cpp
    src/tools/scene_tools.cpp
    src/tools/script_tools.cpp
    src/tools/runtime_tools.cpp
    src/tools/asset_tools.cpp
    src/tools/project_tools.cpp
    src/tools/signal_tools.cpp
    src/tools/physics_nav_tools.cpp
    src/tools/tilemap_grid_tools.cpp
    src/tools/editor_tools.cpp
    src/tools/deep_domain_tools.cpp
)

find_package(Threads REQUIRED)

add_library(didi_core STATIC ${CORE_SOURCES})
set_target_properties(didi_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(didi_core PUBLIC ${DIDI_INCLUDE_DIRECTORIES})
target_link_libraries(didi_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(didi_core PUBLIC bcrypt)
endif()

# 1. Standalone MCP Server Executable (didi.exe)
add_executable(didi src/standalone/main.cpp)
target_link_libraries(didi PRIVATE didi_core)

# 2. GDExtension Shared Library (didi_extension.dll)
set(GDEXT_SOURCES
    src/gdextension/gdextension_entry.cpp
    src/gdextension/editor_hook.cpp
    src/gdextension/godot_bridge.cpp
    src/gdextension/runtime_bridge.cpp
    src/gdextension/viewport_renderer.cpp
    src/gdextension/visual_test_lab.cpp
    src/gdextension/gdextension_ipc.cpp
)

add_library(didi_extension SHARED ${GDEXT_SOURCES})
target_link_libraries(didi_extension PRIVATE didi_core)

# Test-only build of the real extension. It shares every production source but enables
# deterministic failure seams used by the Godot 4.7.2 raw-bridge fixture.
add_library(didi_extension_signal_tests SHARED ${GDEXT_SOURCES})
target_link_libraries(didi_extension_signal_tests PRIVATE didi_core)
target_compile_definitions(didi_extension_signal_tests PRIVATE DIDI_PHASE7_SIGNAL_TEST_SEAMS=1)
set_target_properties(didi_extension_signal_tests PROPERTIES OUTPUT_NAME didi_extension_signal_tests)

if(DIDI_BUILD_TESTS)
    add_executable(phase7_signal_bridge_probe tests/phase7_signal_bridge_probe.cpp)
    target_link_libraries(phase7_signal_bridge_probe PRIVATE didi_core)
endif()

# Copy DLL to addons/didi/bin after build
add_custom_command(TARGET didi_extension POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/addons/didi/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:didi_extension>" "${CMAKE_CURRENT_SOURCE_DIR}/addons/didi/bin/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/demo/addons/didi/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:didi_extension>" "${CMAKE_CURRENT_SOURCE_DIR}/demo/addons/didi/bin/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/tests/godot_smoke/addons/didi/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:didi_extension>" "${CMAKE_CURRENT_SOURCE_DIR}/tests/godot_smoke/addons/didi/bin/"
    COMMENT "Copying didi_extension library to addon directories..."
)

# The standalone server and the extension are the installable products. Without
# these, `cmake --install` did nothing at all.
if(DIDI_CLASS_REFERENCE)
    add_custom_command(TARGET didi POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DIDI_CLASS_REFERENCE}" "$<TARGET_FILE_DIR:didi>/didi_class_reference.json"
        COMMENT "Placing the class reference next to didi"
        VERBATIM
    )
endif()

include(GNUInstallDirs)
install(TARGETS didi
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(DIDI_CLASS_REFERENCE)
    install(FILES "${DIDI_CLASS_REFERENCE}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS didi_extension
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/didi/addons/didi/bin
        RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/didi/addons/didi/bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/addons/didi/plugin.cfg"
              "${CMAKE_CURRENT_SOURCE_DIR}/addons/didi/didi.gdextension"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/didi/addons/didi)

# 3. Unit Test Suite (didi_tests.exe)
set(TEST_SOURCES
    tests/test_main.cpp
    tests/test_jsonrpc.cpp
    tests/test_ipc.cpp
    tests/test_runtime_sessions.cpp
    tests/test_runtime_routing.cpp
    tests/test_runtime_logs.cpp
    tests/test_runtime_output.cpp
    tests/test_expression_sandbox.cpp
    tests/test_tools.cpp
    tests/test_script_patch.cpp
    tests/test_resource_indexer.cpp
    tests/test_project_search.cpp
    tests/test_image_diff.cpp
    tests/test_test_runner.cpp
    tests/test_phase5.cpp
    tests/test_phase6.cpp
    tests/test_tool_manifest.cpp
    tests/test_phase7_contract.cpp
    tests/test_phase7a_signals.cpp
    tests/test_phase7_signal_followup.cpp
    tests/test_phase7_signal_admission.cpp
    tests/test_phase7a_viewport.cpp
    tests/test_phase7a_tile_grid.cpp
    tests/test_phase7b_physics.cpp
    tests/test_phase7b_navigation.cpp
    tests/test_phase7b_animation.cpp
    tests/test_phase7c_input.cpp
    tests/test_phase7c_diagnostics.cpp
    tests/test_phase7c_profiler.cpp
)

if(DIDI_BUILD_TESTS)
    add_executable(didi_tests ${TEST_SOURCES} ${GDEXT_SOURCES})
    target_link_libraries(didi_tests PRIVATE didi_core)
    # The reflection test reads the reference the same way the server does, so
    # it has to exist next to the test binary even when only this target builds.
    if(DIDI_CLASS_REFERENCE)
        add_custom_command(TARGET didi_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DIDI_CLASS_REFERENCE}" "$<TARGET_FILE_DIR:didi_tests>/didi_class_reference.json"
            VERBATIM
        )
    endif()

    # ctest --output-on-failure now works instead of the build directory holding
    # an empty test set while CI ran the binary by hard-coded path.
    enable_testing()
    add_test(NAME didi_tests COMMAND didi_tests)
    # The suite resolves project fixtures relative to the working directory, and
    # CI runs it from the repository root. Say so rather than depending on where
    # ctest happens to be invoked from.
    set_tests_properties(didi_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    if(Python3_Interpreter_FOUND)
        # Discovery rather than a module list, so this cannot drift from the
        # list CI names explicitly. tests/ is not a package, so it has to be the
        # top level directory too.
        add_test(NAME didi_python_tests
                 COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                         -s tests -t tests -p "test_*.py"
                 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endif()
