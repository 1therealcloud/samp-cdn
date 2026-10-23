cmake_minimum_required(VERSION 3.12)
project(sampCDN LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Compile with an x86 toolchain (Linux: g++ -m32).")
endif()

set(SAMP_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/samp-plugin-sdk")

add_library(sampCDN SHARED
    source/main.cpp
    source/artwork.cpp
    source/http.cpp
    source/config.cpp
    source/fixes.cpp
    source/index_hook.cpp
    
    ${SAMP_SDK_DIR}/amxplugin.cpp
    ${SAMP_SDK_DIR}/amxplugin2.cpp
)

target_include_directories(sampCDN PRIVATE
    source
    include
    ${SAMP_SDK_DIR}
)

if(WIN32)
    target_link_libraries(sampCDN PRIVATE ws2_32 mswsock)
    target_compile_definitions(sampCDN PRIVATE _WIN32_WINNT=0x0601)
    target_sources(sampCDN PRIVATE plugin.def)
    set_target_properties(sampCDN PROPERTIES PREFIX "" SUFFIX ".dll")
else()
    target_compile_definitions(sampCDN PRIVATE LINUX)
    target_compile_options(sampCDN PRIVATE -fvisibility=hidden -fno-omit-frame-pointer)
    target_link_libraries(sampCDN PRIVATE pthread dl)
    set_target_properties(sampCDN PROPERTIES PREFIX "" SUFFIX ".so")
endif()
