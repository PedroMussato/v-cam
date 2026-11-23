cmake_minimum_required(VERSION 3.20)
project(VCamSource LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the DLL has no VC++ redistributable dependency when loaded by the Frame Server.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(VCamSource SHARED VCamSource.cpp VCamSource.def)
target_compile_definitions(VCamSource PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(VCamSource PRIVATE /W4 /permissive-)
target_link_libraries(VCamSource PRIVATE mfplat mf mfuuid ole32 advapi32 strmiids)

# Native helper used by the C# app to create the virtual camera (needs Windows 11 SDK).
add_library(VCamReg SHARED VCamReg.cpp)
target_compile_definitions(VCamReg PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(VCamReg PRIVATE /W4 /permissive-)
target_link_libraries(VCamReg PRIVATE mfplat mf mfuuid mfsensorgroup ole32)
