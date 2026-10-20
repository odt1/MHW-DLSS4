cmake_minimum_required(VERSION 3.24)

project(MHW-DLSS4 VERSION 0.9.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# The game loads plugins with LoadLibrary; static CRT keeps the plugin self-contained.
# nvsdk_ngx_s.lib is built against the static CRT as well.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(DLSS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dlss_sdk")
set(NIS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nis")

# The sharpening shaders are compiled with fxc from the Windows SDK into headers: NVSharpen once
# per HDR mode plus the D3D12 root signature, which the Vanilla and RCAS shaders share.
find_program(FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}x64" REQUIRED)
set(SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(NIS_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/nis_sharpen.hlsl")
add_custom_command(
  OUTPUT "${SHADER_DIR}/nis_hdr_cs.h" "${SHADER_DIR}/nis_sdr_cs.h" "${SHADER_DIR}/nis_rs.h"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_DIR}"
  COMMAND "${FXC}" /nologo /T cs_5_0 /E main /O3 /D NIS_HDR_MODE=1 /I "${NIS_DIR}"
          /Fh "${SHADER_DIR}/nis_hdr_cs.h" /Vn kNisHdrCs "${NIS_HLSL}"
  COMMAND "${FXC}" /nologo /T cs_5_0 /E main /O3 /D NIS_HDR_MODE=0 /I "${NIS_DIR}"
          /Fh "${SHADER_DIR}/nis_sdr_cs.h" /Vn kNisSdrCs "${NIS_HLSL}"
  COMMAND "${FXC}" /nologo /T rootsig_1_0 /E RootSig /I "${NIS_DIR}"
          /Fh "${SHADER_DIR}/nis_rs.h" /Vn kNisRootSig "${NIS_HLSL}"
  DEPENDS "${NIS_HLSL}" "${NIS_DIR}/NIS_Main.hlsl" "${NIS_DIR}/NIS_Scaler.h"
  COMMENT "Compiling nis_sharpen.hlsl"
  VERBATIM)
foreach(SHADER vanilla_sharpen rcas)
  add_custom_command(
    OUTPUT "${SHADER_DIR}/${SHADER}_cs.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_DIR}"
    COMMAND "${FXC}" /nologo /T cs_5_0 /E main /O3
            /Fh "${SHADER_DIR}/${SHADER}_cs.h" /Vn "k_${SHADER}_cs"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${SHADER}.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${SHADER}.hlsl"
    COMMENT "Compiling ${SHADER}.hlsl"
    VERBATIM)
endforeach()

add_library(MHW-DLSS4 SHARED
  src/dllmain.cpp
  src/config.cpp
  src/crash.cpp
  src/d3d_util.cpp
  src/dlss_dll.cpp
  src/game.cpp
  src/log.cpp
  src/memory.cpp
  src/ngx_params.cpp
  src/ngx_shim.cpp
  src/probe.cpp
  src/reflex.cpp
  src/sharpen.cpp
  "${SHADER_DIR}/nis_hdr_cs.h"
  "${SHADER_DIR}/nis_sdr_cs.h"
  "${SHADER_DIR}/nis_rs.h"
  "${SHADER_DIR}/vanilla_sharpen_cs.h"
  "${SHADER_DIR}/rcas_cs.h"
)

target_include_directories(MHW-DLSS4 PRIVATE src "${DLSS_SDK_DIR}/include" "${NIS_DIR}" "${SHADER_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvapi")
target_compile_definitions(MHW-DLSS4 PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
  MHW_DLSS4_VERSION="${PROJECT_VERSION}"
  $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
# Optimisation is set here as well, not only through CMAKE_CXX_FLAGS_<CONFIG>: a build folder first
# configured without the MSVC environment caches those as empty, and its builds come out unoptimised
# (0.7.3 and 0.8.1 were). /Zi and /DEBUG keep a PDB for crash dumps in every configuration.
target_compile_options(MHW-DLSS4 PRIVATE /W4 /permissive- /utf-8 /EHsc /Zi "$<$<NOT:$<CONFIG:Debug>>:/O2;/Gw>")
target_link_options(MHW-DLSS4 PRIVATE /DEBUG /OPT:REF /OPT:ICF)

target_link_libraries(MHW-DLSS4 PRIVATE
  "${DLSS_SDK_DIR}/lib/$<IF:$<CONFIG:Debug>,nvsdk_ngx_s_dbg.lib,nvsdk_ngx_s.lib>"
  version)
