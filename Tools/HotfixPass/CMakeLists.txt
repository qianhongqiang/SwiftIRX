cmake_minimum_required(VERSION 3.24)

project(HotfixPass LANGUAGES C CXX OBJCXX)

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

add_executable(
  HotfixPatchTool
  Sources/HotfixPatchTool.cpp
  Sources/HFIRLowering.cpp
)
target_compile_features(HotfixPatchTool PRIVATE cxx_std_20)
target_include_directories(HotfixPatchTool SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(
  HotfixPatchTool
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../SDK/IRHotfixSDK/ABI"
)
target_link_libraries(HotfixPatchTool PRIVATE LLVM)

add_library(
  IRHotfixFormat STATIC
  ../../SDK/IRHotfixSDK/Format/HFIR.cpp
  ../../SDK/IRHotfixSDK/Format/HFPatchContainer.cpp
)
target_compile_features(IRHotfixFormat PUBLIC cxx_std_20)
target_include_directories(
  IRHotfixFormat
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../SDK/IRHotfixSDK/Format"
)
target_link_libraries(HotfixPatchTool PRIVATE IRHotfixFormat)

add_executable(HotfixPackageTool Sources/HotfixPackageTool.cpp)
target_compile_features(HotfixPackageTool PRIVATE cxx_std_20)
target_include_directories(
  HotfixPackageTool
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../SDK/IRHotfixSDK/ABI"
)
target_link_libraries(HotfixPackageTool PRIVATE IRHotfixFormat)

add_executable(HFIRFormatTests Tests/HFIRFormatTests.cpp)
target_compile_features(HFIRFormatTests PRIVATE cxx_std_20)
target_include_directories(
  HFIRFormatTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../SDK/IRHotfixSDK/ABI"
)
target_link_libraries(HFIRFormatTests PRIVATE IRHotfixFormat)

if(APPLE)
  add_executable(
    HFIRVMTests
    Tests/HFIRVMTests.cpp
    ../../SDK/IRHotfixSDK/VM/HFIRRuntime.cpp
    ../../SDK/IRHotfixSDK/Bridge/IRHotfixObjCBridge.mm
    ../../SDK/IRHotfixSDK/HostAdapter/HFHostAdapterRegistry.cpp
  )
  target_compile_features(HFIRVMTests PRIVATE cxx_std_20)
  target_compile_options(
    HFIRVMTests
    PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_include_directories(
    HFIRVMTests
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/../../SDK/IRHotfixSDK/VM"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../SDK/IRHotfixSDK/Bridge"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../SDK/IRHotfixSDK/HostAdapter"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../SDK/IRHotfixSDK/ABI"
  )
  target_link_libraries(HFIRVMTests PRIVATE IRHotfixFormat "-framework Foundation")
endif()

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

set_target_properties(
  HotfixPackageTool
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${HOTFIX_PASS_OUTPUT_DIRECTORY}"
)

set_target_properties(
  HFIRFormatTests
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${HOTFIX_PASS_OUTPUT_DIRECTORY}"
)

if(APPLE)
  set_target_properties(
    HFIRVMTests
    PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${HOTFIX_PASS_OUTPUT_DIRECTORY}"
  )
endif()

if(APPLE)
  target_link_options(HotfixPass PRIVATE "LINKER:-undefined,dynamic_lookup")
endif()
