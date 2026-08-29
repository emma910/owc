cmake_minimum_required(VERSION 3.16)
project(OverwatcheatCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "This port targets Windows only (win32 + DXGI/D3D11 + Interception).")
endif()

set(OWC_INTERCEPTION "${CMAKE_CURRENT_SOURCE_DIR}/Interception/library")

add_executable(Overwatcheat
    src/config.cpp
    src/native.cpp
    src/capture.cpp
    src/gpu_capture.cpp
    src/aim_bot.cpp
    src/main.cpp
)

target_include_directories(Overwatcheat PRIVATE src "${OWC_INTERCEPTION}")

set_target_properties(Overwatcheat PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out"
)

add_custom_command(TARGET Overwatcheat POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OWC_INTERCEPTION}/x64/interception.dll" "$<TARGET_FILE_DIR:Overwatcheat>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../owc.cfg" "$<TARGET_FILE_DIR:Overwatcheat>"
)

if(MSVC)
    target_compile_options(Overwatcheat PRIVATE /O2 /arch:AVX2 /W3 /D_CRT_SECURE_NO_WARNINGS)
    target_link_libraries(Overwatcheat PRIVATE
        "${OWC_INTERCEPTION}/x64/interception.lib"
        user32 gdi32 dxgi d3d11 d3dcompiler winmm
    )
else()
    message(FATAL_ERROR "Only the MSVC toolchain is supported by this port.")
endif()