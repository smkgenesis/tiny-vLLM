cmake_minimum_required(VERSION 3.28)
project(tiny_vllm LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TINY_VLLM_ENABLE_RTL "Build the synthetic RTL backend" OFF)
option(TINY_VLLM_ENABLE_AXI "Build the external AXI RAM backend" OFF)
option(TINY_VLLM_ENABLE_TARGET "Build the standard-port custom RTL adapter" OFF)
set(TINY_VLLM_TARGET_SOURCES "rtl/template/tiny_vllm_target.sv" CACHE STRING
    "Semicolon-separated Verilog sources; top module must be tiny_vllm_target")
include(CTest)
find_package(yaml-cpp 0.8 CONFIG REQUIRED)
if(BUILD_TESTING)
    find_package(Catch2 3.4 CONFIG REQUIRED)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    include(Catch)
endif()
if(TINY_VLLM_ENABLE_RTL OR TINY_VLLM_ENABLE_AXI OR TINY_VLLM_ENABLE_TARGET)
    find_package(verilator 5.020 CONFIG REQUIRED HINTS $ENV{VERILATOR_ROOT})
endif()

execute_process(COMMAND git rev-parse --verify HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE SOURCE_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE git_status)
if(NOT git_status EQUAL 0)
    set(SOURCE_REVISION "uncommitted")
endif()
execute_process(COMMAND git status --porcelain WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE source_changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE git_status)
set(SOURCE_DIRTY "false")
if(NOT git_status EQUAL 0)
    set(SOURCE_DIRTY "unknown")
elseif(source_changes)
    set(SOURCE_DIRTY "true")
endif()

add_library(tiny-vllm-core src/config.cpp src/core.cpp)
target_include_directories(tiny-vllm-core PUBLIC include)
target_link_libraries(tiny-vllm-core PUBLIC yaml-cpp::yaml-cpp)
target_compile_options(tiny-vllm-core PRIVATE -Wall -Wextra -Wpedantic)

function(add_cli name library)
    add_executable(${name} src/main.cpp)
    target_link_libraries(${name} PRIVATE ${library})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(${name} PRIVATE ${ARGN}
        TINY_VLLM_SOURCE_REVISION="${SOURCE_REVISION}" TINY_VLLM_SOURCE_DIRTY="${SOURCE_DIRTY}")
endfunction()
add_cli(tiny-vllm-trace tiny-vllm-core)

if(BUILD_TESTING)
    add_executable(tiny-vllm-core-tests tests/core_tests.cpp)
    target_link_libraries(tiny-vllm-core-tests PRIVATE tiny-vllm-core Catch2::Catch2WithMain)
    catch_discover_tests(tiny-vllm-core-tests TEST_PREFIX "core.")
    add_test(NAME trace_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/trace_cli.py"
        "$<TARGET_FILE:tiny-vllm-trace>" "${CMAKE_CURRENT_SOURCE_DIR}/examples/basic.yaml")
endif()

if(TINY_VLLM_ENABLE_RTL)
    add_library(tiny-vllm-rtl-backend src/backends/rtl.cpp)
    target_link_libraries(tiny-vllm-rtl-backend PUBLIC tiny-vllm-core)
    verilate(tiny-vllm-rtl-backend SOURCES rtl/reference/single_engine.sv
        TOP_MODULE single_engine PREFIX Vsingle_engine TRACE VERILATOR_ARGS --Wall)
    verilate(tiny-vllm-rtl-backend SOURCES rtl/reference/split_rw.sv rtl/reference/single_engine.sv
        TOP_MODULE split_rw PREFIX Vsplit_rw TRACE VERILATOR_ARGS --Wall)
    add_cli(tiny-vllm-rtl tiny-vllm-rtl-backend TINY_VLLM_RTL)
    if(BUILD_TESTING)
        add_executable(tiny-vllm-rtl-tests tests/rtl_service.cpp)
        target_link_libraries(tiny-vllm-rtl-tests PRIVATE tiny-vllm-rtl-backend Catch2::Catch2WithMain)
        catch_discover_tests(tiny-vllm-rtl-tests TEST_PREFIX "rtl.")
        add_test(NAME rtl_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/rtl_cli.py"
            "$<TARGET_FILE:tiny-vllm-rtl>" "$<TARGET_FILE:tiny-vllm-trace>" "${CMAKE_CURRENT_SOURCE_DIR}/examples/basic.yaml")
    endif()
endif()

if(TINY_VLLM_ENABLE_AXI)
    set(AXI_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/rtl/vendor/verilog-axi/axi_ram.v")
    file(SHA256 "${AXI_SOURCE}" AXI_HASH)
    if(NOT AXI_HASH STREQUAL "2eb669f3cd1fd912a74c024df3bea5765a3599eead1a007c63f19b6b185d55c4")
        message(FATAL_ERROR "axi_ram.v differs from pinned upstream")
    endif()
    # Avoid Verilator 5.020's escaping of non-ASCII absolute source paths.
    configure_file("${AXI_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/external-rtl/axi_ram.v" COPYONLY)
    add_library(tiny-vllm-axi-backend src/backends/axi.cpp)
    target_link_libraries(tiny-vllm-axi-backend PUBLIC tiny-vllm-core)
    target_compile_options(tiny-vllm-axi-backend PRIVATE -Wall -Wextra -Wpedantic)
    verilate(tiny-vllm-axi-backend SOURCES "${CMAKE_CURRENT_BINARY_DIR}/external-rtl/axi_ram.v"
        TOP_MODULE axi_ram PREFIX Vaxi_ram
        # Only reviewed warnings from the unchanged vendor source are disabled.
        VERILATOR_ARGS --Wall -Wno-WIDTH -Wno-UNUSEDSIGNAL -Wno-CASEINCOMPLETE
        -GDATA_WIDTH=64 -GADDR_WIDTH=20 -GID_WIDTH=64 -GPIPELINE_OUTPUT=0)
    add_cli(tiny-vllm-axi tiny-vllm-axi-backend TINY_VLLM_AXI)
    if(BUILD_TESTING)
        add_executable(tiny-vllm-axi-tests tests/axi_backend.cpp)
        target_link_libraries(tiny-vllm-axi-tests PRIVATE tiny-vllm-axi-backend Catch2::Catch2WithMain)
        catch_discover_tests(tiny-vllm-axi-tests TEST_PREFIX "axi.")
        add_test(NAME axi_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_axi_validation.py"
            --axi "$<TARGET_FILE:tiny-vllm-axi>" --trace "$<TARGET_FILE:tiny-vllm-trace>"
            --examples "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    endif()
endif()

if(TINY_VLLM_ENABLE_TARGET)
    # Stage sources in the ASCII build tree, also preserving sibling includes.
    set(target_sources "")
    set(target_names "")
    set(target_manifest "")
    foreach(input IN LISTS TINY_VLLM_TARGET_SOURCES)
        get_filename_component(absolute "${input}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        get_filename_component(name "${absolute}" NAME)
        if(name IN_LIST target_names OR NOT EXISTS "${absolute}")
            message(FATAL_ERROR "Missing custom RTL source or duplicate basename: ${input}")
        endif()
        list(APPEND target_names "${name}")
        file(SHA256 "${absolute}" hash)
        string(APPEND target_manifest "${name} ${hash}\n")
        configure_file("${absolute}" "${CMAKE_CURRENT_BINARY_DIR}/target-rtl/${name}" COPYONLY)
        if(name MATCHES "\\.(sv|v)$")
            list(APPEND target_sources "${CMAKE_CURRENT_BINARY_DIR}/target-rtl/${name}")
        endif()
    endforeach()
    if(NOT target_sources)
        message(FATAL_ERROR "TINY_VLLM_TARGET_SOURCES needs at least one .sv or .v file")
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/target-sources.txt" "${target_manifest}")
    string(SHA256 target_hash "${target_manifest}")
    add_library(tiny-vllm-target-backend src/backends/target.cpp)
    target_link_libraries(tiny-vllm-target-backend PUBLIC tiny-vllm-core)
    # The public top name is fixed, but user filenames and source grouping are not.
    verilate(tiny-vllm-target-backend SOURCES ${target_sources} TOP_MODULE tiny_vllm_target
        PREFIX Vtiny_vllm_target TRACE INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/target-rtl"
        VERILATOR_ARGS --Wall -Wno-DECLFILENAME)
    add_cli(tiny-vllm-target tiny-vllm-target-backend TINY_VLLM_TARGET TINY_VLLM_TARGET_SOURCE_HASH="${target_hash}")
    # These assertions are for the bundled template, not an arbitrary user DUT.
    if(BUILD_TESTING AND TINY_VLLM_TARGET_SOURCES STREQUAL "rtl/template/tiny_vllm_target.sv")
        add_executable(tiny-vllm-target-tests tests/target.cpp)
        target_link_libraries(tiny-vllm-target-tests PRIVATE tiny-vllm-target-backend Catch2::Catch2WithMain)
        catch_discover_tests(tiny-vllm-target-tests TEST_PREFIX "target.")
        add_test(NAME target_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/target_cli.py"
            "$<TARGET_FILE:tiny-vllm-target>" "$<TARGET_FILE:tiny-vllm-trace>" "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    endif()
endif()
