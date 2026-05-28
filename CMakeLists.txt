cmake_minimum_required(VERSION 3.12)
project(Write_MMTS LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EDCB_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/EDCB")
set(WRITE_MMTS_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Configure output directories for built binaries
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Setup common compile definitions
set(COMMON_DEFS
    UNICODE
    _UNICODE
)

# 1. Target: Write_MMTS_OneService (statically incorporates Write_OneService source code)
add_library(Write_MMTS_OneService SHARED
    src/Write_MMTS.cpp
    src/Write_PlugIn.def
    ${EDCB_VENDOR_DIR}/Write_OneService/Write_OneService/stdafx.cpp
    ${EDCB_VENDOR_DIR}/Write_OneService/Write_OneService/WriteMain.cpp
    ${EDCB_VENDOR_DIR}/Common/PathUtil.cpp
    ${EDCB_VENDOR_DIR}/Common/StringUtil.cpp
    ${EDCB_VENDOR_DIR}/Common/TimeUtil.cpp
    ${EDCB_VENDOR_DIR}/Common/TSBuffUtil.cpp
    ${EDCB_VENDOR_DIR}/Common/TSPacketUtil.cpp
    ${EDCB_VENDOR_DIR}/Common/WritePlugInUtil.cpp
    ${EDCB_VENDOR_DIR}/Common/EpgTimerUtil.cpp
    ${EDCB_VENDOR_DIR}/BonCtrl/CATUtil.cpp
    ${EDCB_VENDOR_DIR}/BonCtrl/CreatePATPacket.cpp
    ${EDCB_VENDOR_DIR}/BonCtrl/CreatePMTPacket.cpp
    ${EDCB_VENDOR_DIR}/BonCtrl/PacketInit.cpp
    ${EDCB_VENDOR_DIR}/BonCtrl/PMTUtil.cpp
    ${EDCB_VENDOR_DIR}/BonCtrl/ServiceFilter.cpp
)
target_compile_definitions(Write_MMTS_OneService PRIVATE
    ${COMMON_DEFS}
    USE_ONSERVICE
    PLUGIN_NAME=L"MMTS / OneService ハイブリッド保存 PlugIn"
)
target_include_directories(Write_MMTS_OneService PRIVATE
    ${EDCB_VENDOR_DIR}/Write_OneService/Write_OneService
    ${EDCB_VENDOR_DIR}/Common
    ${EDCB_VENDOR_DIR}/BonCtrl
    ${WRITE_MMTS_SRC_DIR}
)
target_compile_options(Write_MMTS_OneService PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# 2. Target: Write_MMTS_Default (statically incorporates Write_Default source code)
add_library(Write_MMTS_Default SHARED
    src/Write_MMTS.cpp
    src/Write_PlugIn.def
    ${EDCB_VENDOR_DIR}/Write_Default/Write_Default/stdafx.cpp
    ${EDCB_VENDOR_DIR}/Write_Default/Write_Default/WriteMain.cpp
    ${EDCB_VENDOR_DIR}/Write_Default/Write_Default/SettingDlg.cpp
    ${EDCB_VENDOR_DIR}/Write_Default/Write_Default/Write_Default.rc
    ${EDCB_VENDOR_DIR}/Common/PathUtil.cpp
    ${EDCB_VENDOR_DIR}/Common/StringUtil.cpp
    ${EDCB_VENDOR_DIR}/Common/TimeUtil.cpp
)
target_compile_definitions(Write_MMTS_Default PRIVATE
    ${COMMON_DEFS}
    PLUGIN_NAME=L"MMTS / デフォルトTS ハイブリッド保存 PlugIn"
)
target_include_directories(Write_MMTS_Default PRIVATE
    ${EDCB_VENDOR_DIR}/Write_Default/Write_Default
    ${EDCB_VENDOR_DIR}/Common
    ${WRITE_MMTS_SRC_DIR}
)
target_compile_options(Write_MMTS_Default PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# Use MT/MTd runtime library (static linking for VC++ runtime)
if(MSVC)
    set_property(TARGET Write_MMTS_OneService PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET Write_MMTS_Default PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
