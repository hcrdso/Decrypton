cmake_minimum_required(VERSION 3.20)

project(
    decrypton
    VERSION 0.2.0
    DESCRIPTION "Windows x64 PE memory image dumper and import rebuilder"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "decrypton is Windows-only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "decrypton must be built as a 64-bit executable")
endif()

add_executable(decrypton main.cpp)
target_compile_features(decrypton PRIVATE cxx_std_20)
target_compile_definitions(
    decrypton
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
)

if(MSVC)
    target_compile_options(
        decrypton
        PRIVATE
            /W4
            /permissive-
            /EHsc
            /utf-8
            /sdl
            /guard:cf
    )
    target_link_options(decrypton PRIVATE /DYNAMICBASE /NXCOMPAT /guard:cf)
else()
    target_compile_options(
        decrypton
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
    )
endif()

set_target_properties(
    decrypton
    PROPERTIES
        OUTPUT_NAME "decrypton"
        CXX_EXTENSIONS OFF
)

install(TARGETS decrypton RUNTIME DESTINATION bin)
