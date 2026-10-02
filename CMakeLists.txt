cmake_minimum_required(VERSION 3.28)
project(tiny_vllm LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TINY_VLLM_ENABLE_RTL "Build the synthetic RTL backend" OFF)
option(TINY_VLLM_ENABLE_AXI "Build the external AXI RAM backend" OFF)
include(CTest)
find_package(yaml-cpp 0.8 CONFIG REQUIRED)
if(BUILD_TESTING)
    find_package(Catch2 3.4 CONFIG REQUIRED)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    include(Catch)
endif()
if(TINY_VLLM_ENABLE_RTL OR TINY_VLLM_ENABLE_AXI)
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
        "$<TARGET_FILE:tiny-vllm-trace>" "${CMAKE_CURRENT_SOURCE_DIR}/examples/small.yaml")
endif()

if(TINY_VLLM_ENABLE_RTL)
    add_library(tiny-vllm-rtl-backend src/rtl_backend.cpp)
    target_link_libraries(tiny-vllm-rtl-backend PUBLIC tiny-vllm-core)
    verilate(tiny-vllm-rtl-backend SOURCES rtl/kv_service.sv
        TOP_MODULE kv_service PREFIX Vkv_service TRACE VERILATOR_ARGS --Wall)
    verilate(tiny-vllm-rtl-backend SOURCES rtl/kv_service_dual.sv rtl/kv_service.sv
        TOP_MODULE kv_service_dual PREFIX Vkv_service_dual TRACE VERILATOR_ARGS --Wall)
    add_cli(tiny-vllm-rtl tiny-vllm-rtl-backend TINY_VLLM_RTL)
    if(BUILD_TESTING)
        add_executable(tiny-vllm-rtl-tests tests/rtl_service.cpp)
        target_link_libraries(tiny-vllm-rtl-tests PRIVATE tiny-vllm-rtl-backend Catch2::Catch2WithMain)
        catch_discover_tests(tiny-vllm-rtl-tests TEST_PREFIX "rtl.")
        add_test(NAME rtl_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/rtl_cli.py"
            "$<TARGET_FILE:tiny-vllm-rtl>" "$<TARGET_FILE:tiny-vllm-trace>" "${CMAKE_CURRENT_SOURCE_DIR}/examples/small.yaml")
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
    add_library(tiny-vllm-axi-backend src/axi_backend.cpp)
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
