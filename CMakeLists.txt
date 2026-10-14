cmake_minimum_required(VERSION 3.24)
project(BloodborneIOSLab VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
option(BB_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
set(BB_BUILD_ID "local-unversioned" CACHE STRING "Build revision")
if(NOT BB_BUILD_ID MATCHES "^[A-Za-z0-9._-]+$")
    message(FATAL_ERROR "BB_BUILD_ID contains unsupported characters")
endif()
add_compile_definitions(BB_BUILD_ID="${BB_BUILD_ID}")
if(MSVC)
    add_compile_options(/W4 /WX /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()
if(BB_SANITIZERS)
    if(MSVC OR WIN32)
        message(FATAL_ERROR "Use Clang/GCC on Linux or macOS for this sanitizer configuration")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(ALL_FIXTURES
    add sub xor
    add8 add16 add32 add64 adc8 adc64 sub8 sub16 sub32 sub64 sbb64 and8 or64 xor8 xor64 shl8 shr64 cmp32 test64
    call_direct call_indirect recursion jump_table vtable_call callback
    abi_0args abi_1arg abi_6args abi_12args abi_struct_byval abi_struct_ret abi_fp_args abi_align16 abi_callee_saved)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if(NOT BB_GENERATED_DIR)
        message(FATAL_ERROR "Set BB_GENERATED_DIR to fixtures produced by a host build")
    endif()
    set(generated_dir "${BB_GENERATED_DIR}")
    foreach(name ${ALL_FIXTURES})
        if(NOT EXISTS "${generated_dir}/${name}.cpp")
            message(FATAL_ERROR "Missing translated fixture: ${name}.cpp")
        endif()
    endforeach()
else()
    add_library(bb_decoder STATIC tools/aot.cpp)
    target_include_directories(bb_decoder PUBLIC tools core)
    add_executable(bb-aot tools/aot_main.cpp)
    target_link_libraries(bb-aot PRIVATE bb_decoder)
    set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${generated_dir}")
    foreach(name ${ALL_FIXTURES})
        add_custom_command(OUTPUT "${generated_dir}/${name}.cpp"
            COMMAND bb-aot "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${name}.hex" "fixture_${name}" "${generated_dir}/${name}.cpp"
            DEPENDS bb-aot "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${name}.hex"
            VERBATIM)
    endforeach()
endif()

set(LIBATRAC9_SOURCES
    third_party/LibAtrac9/src/band_extension.c
    third_party/LibAtrac9/src/bit_allocation.c
    third_party/LibAtrac9/src/bit_reader.c
    third_party/LibAtrac9/src/decinit.c
    third_party/LibAtrac9/src/decoder.c
    third_party/LibAtrac9/src/huffCodes.c
    third_party/LibAtrac9/src/imdct.c
    third_party/LibAtrac9/src/libatrac9.c
    third_party/LibAtrac9/src/quantization.c
    third_party/LibAtrac9/src/scale_factors.c
    third_party/LibAtrac9/src/tables.c
    third_party/LibAtrac9/src/unpack.c
    third_party/LibAtrac9/src/utility.c)
add_library(bb_atrac9 STATIC ${LIBATRAC9_SOURCES})
target_include_directories(bb_atrac9 PUBLIC third_party/LibAtrac9/src)
set_target_properties(bb_atrac9 PROPERTIES C_STANDARD 99)
# libatrac9.h usa __declspec(dllimport) em MSVC salvo COMPILING_DLL; a lib e estatica,
# entao a definicao precisa valer tambem para quem linka (PUBLIC), sem editar o upstream.
target_compile_definitions(bb_atrac9 PUBLIC COMPILING_DLL)
if(MSVC)
    target_compile_options(bb_atrac9 PRIVATE /w)
else()
    target_compile_options(bb_atrac9 PRIVATE -w)
endif()

set(CORE_SOURCES
    core/guest_memory.cpp
    core/smoke.cpp
    core/dispatcher.cpp
    core/hle.cpp
    core/kernel_threads.cpp
    core/manifest.cpp
    core/elf_parser.cpp
    core/game_asset_probe.cpp
    core/guest_loader.cpp
    core/input.cpp
    core/vfs.cpp
    core/binder.cpp
    core/atrac9_decoder.cpp
    core/audio.cpp
    core/stability.cpp
    core/engine_loop.cpp
    core/miniz.c
    core/miniz_tdef.c
    core/miniz_tinfl.c
    core/shader_translator.cpp
    core/metal_pipeline.cpp
    core/flver_parser.cpp)
foreach(name ${ALL_FIXTURES})
    list(APPEND CORE_SOURCES "${generated_dir}/${name}.cpp")
endforeach()

add_library(bb_core STATIC ${CORE_SOURCES})
target_include_directories(bb_core PUBLIC core third_party/LibAtrac9/src)
find_package(Threads REQUIRED)
target_link_libraries(bb_core PUBLIC Threads::Threads bb_atrac9)
if(WIN32)
    target_link_libraries(bb_core PUBLIC psapi)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJCXX)
    add_executable(BBHost MACOSX_BUNDLE host/ios/main.mm)
    target_compile_options(BBHost PRIVATE -fobjc-arc)
    target_link_libraries(BBHost PRIVATE bb_core "-framework UIKit" "-framework MetalKit" "-framework Metal" "-framework GameController" "-framework Foundation" "-framework CoreGraphics")
    set_target_properties(BBHost PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/host/ios/Info.plist"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.bloodborne.lab.BBHost"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
else()
    add_executable(bb-smoke host/main.cpp)
    target_link_libraries(bb-smoke PRIVATE bb_core)
    include(CTest)
    if(BUILD_TESTING)
        foreach(name memory threads alu integration simd dispatcher hle concurrency manifest game_parser guest_boot input vfs binder game_asset_probe audio stability engine shader_translator metal_pipeline flver_parser)
            add_executable(test_${name} tests/test_${name}.cpp)
            target_link_libraries(test_${name} PRIVATE bb_core)
            add_test(NAME ${name} COMMAND test_${name})
        endforeach()
        add_executable(test_aot tests/test_aot.cpp)
        target_link_libraries(test_aot PRIVATE bb_decoder bb_core)
        add_test(NAME aot_rejection COMMAND test_aot)
        add_test(NAME cli_integration COMMAND "${CMAKE_COMMAND}"
            "-DAOT=$<TARGET_FILE:bb-aot>" "-DSMOKE=$<TARGET_FILE:bb-smoke>"
            "-DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/add.hex"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.cmake")
        set_tests_properties(memory threads alu integration simd dispatcher hle concurrency manifest game_parser guest_boot input vfs binder game_asset_probe audio stability engine shader_translator metal_pipeline flver_parser aot_rejection cli_integration PROPERTIES TIMEOUT 60)
    endif()
endif()
