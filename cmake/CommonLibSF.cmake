# CMake wrapper for libxse/CommonLibSF (xmake-only project)
# Builds CommonLibSF + commonlib-shared as static libraries

set(COMMONLIBSF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSF")
set(COMMONLIB_SHARED_ROOT "${COMMONLIBSF_ROOT}/lib/commonlib-shared")

# CommonLibSF embeds the ABSOLUTE dev path into the DLL via __FILE__/std::source_location
# in its report_and_fail / REL / BSTArray asserts. Trim the repo-root prefix from __FILE__
# so the shipped binary carries no dev-machine layout (matches the PULSAR target's flag).
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/" PULSAR_TRIM_ROOT)

# ---- commonlib-shared (static lib) ----
file(GLOB_RECURSE COMMONLIB_SHARED_SOURCES
  "${COMMONLIB_SHARED_ROOT}/src/*.cpp"
)

add_library(commonlib-shared STATIC ${COMMONLIB_SHARED_SOURCES})
target_include_directories(commonlib-shared PUBLIC "${COMMONLIB_SHARED_ROOT}/include")
target_compile_features(commonlib-shared PUBLIC cxx_std_23)
target_compile_definitions(commonlib-shared PUBLIC
  NOMINMAX
  _UNICODE
  UNICODE
  WIN32_LEAN_AND_MEAN
)
target_compile_options(commonlib-shared PUBLIC
  /EHsc /permissive- /bigobj /utf-8
  /Zc:enumTypes /Zc:preprocessor /Zc:templateScope
  /wd4200 /wd4201 /wd4324
  /we4715
)
target_compile_options(commonlib-shared PRIVATE "/d1trimfile:${PULSAR_TRIM_ROOT}")
target_link_libraries(commonlib-shared PUBLIC
  spdlog::spdlog
  advapi32 bcrypt d3d11 d3dcompiler dbghelp dxgi ole32 shell32 user32 version ws2_32
)
target_precompile_headers(commonlib-shared PRIVATE "${COMMONLIB_SHARED_ROOT}/src/REX/PCH.h")

# ---- CommonLibSF (static lib) ----
file(GLOB_RECURSE COMMONLIBSF_SOURCES
  "${COMMONLIBSF_ROOT}/src/*.cpp"
)

add_library(CommonLibSF STATIC ${COMMONLIBSF_SOURCES})
target_include_directories(CommonLibSF PUBLIC "${COMMONLIBSF_ROOT}/include")
target_compile_options(CommonLibSF PRIVATE "/d1trimfile:${PULSAR_TRIM_ROOT}")
target_link_libraries(CommonLibSF PUBLIC commonlib-shared)
target_precompile_headers(CommonLibSF PRIVATE "${COMMONLIBSF_ROOT}/include/SFSE/Impl/PCH.h")

add_library(CommonLibSF::CommonLibSF ALIAS CommonLibSF)

# ---- add_commonlibsf_plugin function ----
function(add_commonlibsf_plugin TARGET)
  set(options USE_ADDRESS_LIBRARY USE_SIGNATURE_SCANNING STRUCT_DEPENDENT)
  set(oneValueArgs AUTHOR NAME VERSION)
  set(multiValueArgs SOURCES COMPATIBLE_RUNTIMES)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  add_library(${TARGET} SHARED ${ARG_SOURCES})
  target_link_libraries(${TARGET} PUBLIC CommonLibSF::CommonLibSF)

  # Generate Plugin.h
  set(PLUGIN_NAME "${TARGET}")
  if(DEFINED ARG_NAME)
    set(PLUGIN_NAME "${ARG_NAME}")
  endif()

  set(PLUGIN_AUTHOR "${ARG_AUTHOR}")
  if(NOT DEFINED ARG_AUTHOR)
    set(PLUGIN_AUTHOR "Unknown")
  endif()

  set(PLUGIN_VERSION "${PROJECT_VERSION}")
  if(DEFINED ARG_VERSION)
    set(PLUGIN_VERSION "${ARG_VERSION}")
  endif()

  string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ver_match "${PLUGIN_VERSION}")
  set(VER_MAJOR "${CMAKE_MATCH_1}")
  set(VER_MINOR "${CMAKE_MATCH_2}")
  set(VER_PATCH "${CMAKE_MATCH_3}")

  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/Plugin.h"
    "#pragma once\n\n"
    "namespace Plugin\n"
    "{\n"
    "    using namespace std::string_view_literals;\n\n"
    "    static constexpr auto Name{ \"${PLUGIN_NAME}\"sv };\n"
    "    static constexpr auto Author{ \"${PLUGIN_AUTHOR}\"sv };\n"
    "    static constexpr auto Version{\n"
    "        REL::Version{ ${VER_MAJOR}, ${VER_MINOR}, ${VER_PATCH}, 0 }\n"
    "    };\n"
    "}\n"
  )

  target_include_directories(${TARGET} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
