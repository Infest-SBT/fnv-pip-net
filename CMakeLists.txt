cmake_minimum_required(VERSION 3.21)
project(PipNet VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "PipNet requires Windows Win32/x86 build (FalloutNV.exe is 32 bit).")
endif()

set(XNVSE_SOURCE_ROOT "" CACHE PATH "Root of checked-out xNVSE/NVSE source tree")
set(WEBVIEW2_SDK_ROOT "" CACHE PATH "Extracted Microsoft.Web.WebView2 NuGet package root")

if(NOT EXISTS "${XNVSE_SOURCE_ROOT}/nvse/nvse/PluginAPI.h")
    message(FATAL_ERROR "Set XNVSE_SOURCE_ROOT to root of https://github.com/xNVSE/NVSE")
endif()
set(WV_INCLUDE "${WEBVIEW2_SDK_ROOT}/build/native/include")
set(WV_LOADER "${WEBVIEW2_SDK_ROOT}/build/native/x86/WebView2LoaderStatic.lib")
if(NOT EXISTS "${WV_INCLUDE}/WebView2.h" OR NOT EXISTS "${WV_LOADER}")
    message(FATAL_ERROR "Set WEBVIEW2_SDK_ROOT to extracted Microsoft.Web.WebView2 NuGet package")
endif()

add_library(PipNet SHARED src/PipNet.cpp src/PipboySurface.cpp)
set_target_properties(PipNet PROPERTIES
    OUTPUT_NAME "PipNet"
    PREFIX ""
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)

target_compile_definitions(PipNet PRIVATE RUNTIME=1 RUNTIME_VERSION=0x040020D0 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(PipNet PRIVATE
    "${XNVSE_SOURCE_ROOT}"
    "${XNVSE_SOURCE_ROOT}/nvse"
    "${WV_INCLUDE}")
target_link_libraries(PipNet PRIVATE
    "${WV_LOADER}"
    ole32 user32 gdi32 shell32 shlwapi version d3d9 dxguid)
if(MSVC)
    target_compile_options(PipNet PRIVATE /W4 /EHsc /FI\"nvse/prefix.h\")
endif()
