cmake_minimum_required(VERSION 3.21)
project(decrypton VERSION 0.4.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Decrypton currently supports Windows only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Configure an x64 build environment")
endif()

add_executable(decrypton
    src/main.cpp
    src/capture.cpp
    src/offline.cpp
)

target_include_directories(decrypton PRIVATE include)
target_compile_features(decrypton PRIVATE cxx_std_20)
target_compile_definitions(decrypton PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)
target_link_libraries(decrypton PRIVATE
    bcrypt
    dbghelp
)

if(MSVC)
    target_compile_options(decrypton PRIVATE
        /W4
        /permissive-
        /EHsc
        /utf-8
        /sdl
        /guard:cf
        /Zc:preprocessor
        /Zc:__cplusplus
    )
    target_link_options(decrypton PRIVATE
        /DYNAMICBASE
        /NXCOMPAT
        /HIGHENTROPYVA
        /GUARD:CF
    )
else()
    target_compile_options(decrypton PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Werror
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME decrypton_version
        COMMAND $<TARGET_FILE:decrypton> --version)
    add_test(NAME decrypton_help
        COMMAND $<TARGET_FILE:decrypton> --help)
    add_test(NAME decrypton_verify_self
        COMMAND $<TARGET_FILE:decrypton> verify $<TARGET_FILE:decrypton>)
    add_test(NAME decrypton_transform_roundtrip
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/transform-roundtrip.ps1
            -Decrypton $<TARGET_FILE:decrypton>
            -WorkingDirectory ${CMAKE_CURRENT_BINARY_DIR}/transform-test)
endif()

install(TARGETS decrypton RUNTIME DESTINATION bin)
