cmake_minimum_required(VERSION 3.20)
project(didi VERSION 1.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/didi/common
    ${CMAKE_CURRENT_SOURCE_DIR}/include/didi/mcp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/didi/offline
    ${CMAKE_CURRENT_SOURCE_DIR}/include/didi/gdextension
)

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
    src/common/ipc_channel_win32.cpp
    src/mcp/jsonrpc.cpp
    src/mcp/mcp_server.cpp
    src/mcp/tool_registry.cpp
    src/mcp/resource_registry.cpp
    src/mcp/prompt_registry.cpp
    src/runtime/session_client.cpp
    src/gdextension/session_host.cpp
    src/gdextension/runtime_log.cpp
    src/gdextension/runtime_request_router.cpp
    src/gdextension/expression_sandbox.cpp
    src/offline/gdscript_diagnostics.cpp
    src/offline/resource_indexer.cpp
    src/offline/project_search.cpp
    src/offline/test_runner.cpp
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
)

find_package(Threads REQUIRED)

add_library(didi_core STATIC ${CORE_SOURCES})
set_target_properties(didi_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
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

# 3. Unit Test Suite (didi_tests.exe)
set(TEST_SOURCES
    tests/test_main.cpp
    tests/test_jsonrpc.cpp
    tests/test_ipc.cpp
    tests/test_runtime_sessions.cpp
    tests/test_runtime_routing.cpp
    tests/test_runtime_logs.cpp
    tests/test_expression_sandbox.cpp
    tests/test_tools.cpp
    tests/test_script_patch.cpp
    tests/test_resource_indexer.cpp
    tests/test_project_search.cpp
    tests/test_image_diff.cpp
)

add_executable(didi_tests ${TEST_SOURCES})
target_link_libraries(didi_tests PRIVATE didi_core)
