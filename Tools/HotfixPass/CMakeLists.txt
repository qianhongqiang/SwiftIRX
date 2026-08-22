cmake_minimum_required(VERSION 3.24)

project(HotfixPass LANGUAGES C CXX)

find_package(
  LLVM 19.1.7 EXACT REQUIRED CONFIG
  PATHS /opt/homebrew/opt/llvm@19/lib/cmake/llvm
  NO_DEFAULT_PATH
)

add_definitions(${LLVM_DEFINITIONS})

add_library(HotfixPass MODULE Sources/HotfixPass.cpp)
target_compile_features(HotfixPass PRIVATE cxx_std_20)
target_include_directories(HotfixPass SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(
  HotfixPass
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../SDK/IRHotfixSDK/ABI"
)

add_executable(HotfixManifestTool Sources/HotfixManifestTool.cpp)
target_compile_features(HotfixManifestTool PRIVATE cxx_std_20)
target_include_directories(HotfixManifestTool SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(
  HotfixManifestTool
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../SDK/IRHotfixSDK/ABI"
)
target_link_libraries(HotfixManifestTool PRIVATE LLVM)

add_executable(HotfixPatchTool Sources/HotfixPatchTool.cpp)
target_compile_features(HotfixPatchTool PRIVATE cxx_std_20)
target_include_directories(HotfixPatchTool SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(
  HotfixPatchTool
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../SDK/IRHotfixSDK/ABI"
)
target_link_libraries(HotfixPatchTool PRIVATE LLVM)

set(
  HOTFIX_PASS_OUTPUT_DIRECTORY
  "${CMAKE_CURRENT_SOURCE_DIR}/.build"
  CACHE PATH
  "Directory for the HotfixPass plugin"
)

set_target_properties(
  HotfixPass
  PROPERTIES
    PREFIX "lib"
    SUFFIX ".dylib"
    LIBRARY_OUTPUT_DIRECTORY "${HOTFIX_PASS_OUTPUT_DIRECTORY}"
)

set_target_properties(
  HotfixManifestTool
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${HOTFIX_PASS_OUTPUT_DIRECTORY}"
)

set_target_properties(
  HotfixPatchTool
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${HOTFIX_PASS_OUTPUT_DIRECTORY}"
)

if(APPLE)
  target_link_options(HotfixPass PRIVATE "LINKER:-undefined,dynamic_lookup")
endif()
