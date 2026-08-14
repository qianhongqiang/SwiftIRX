cmake_minimum_required(VERSION 3.24)

project(HotfixPass LANGUAGES C CXX)

set(SWIFT_LLVM_COMMIT "8f0d2ca924db37c8f8161d55c21b9097b05b72f3")
set(SWIFT_LLVM_VERSION "19.1.5")
set(SWIFT_LLVM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.toolchain/swift-llvm-project")
set(SWIFT_LLVM_GENERATED "${CMAKE_CURRENT_SOURCE_DIR}/.toolchain/generated")

function(run_checked description)
  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${description} failed:\n${output}${error}")
  endif()
endfunction()

find_program(GIT_EXECUTABLE git REQUIRED)
find_program(XCRUN_EXECUTABLE xcrun REQUIRED)

execute_process(
  COMMAND "${XCRUN_EXECUTABLE}" swiftc --version
  RESULT_VARIABLE swift_result
  OUTPUT_VARIABLE swift_output
  ERROR_VARIABLE swift_error
)
string(CONCAT swift_version "${swift_output}" "${swift_error}")
if(NOT swift_result EQUAL 0 OR
   NOT swift_version MATCHES "Apple Swift version 6\\.2\\.4")
  message(FATAL_ERROR
    "HotfixPass requires Apple Swift 6.2.4, found:\n${swift_version}"
  )
endif()

if(NOT EXISTS "${SWIFT_LLVM_ROOT}/.git")
  if(EXISTS "${SWIFT_LLVM_ROOT}")
    message(FATAL_ERROR
      "Remove incomplete toolchain cache at ${SWIFT_LLVM_ROOT} and configure again"
    )
  endif()

  file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.toolchain")
  run_checked("Initializing Swift LLVM cache"
    "${GIT_EXECUTABLE}" init "${SWIFT_LLVM_ROOT}"
  )
  run_checked("Adding Swift LLVM remote"
    "${GIT_EXECUTABLE}" -C "${SWIFT_LLVM_ROOT}" remote add origin
    https://github.com/swiftlang/llvm-project.git
  )
  run_checked("Enabling sparse checkout"
    "${GIT_EXECUTABLE}" -C "${SWIFT_LLVM_ROOT}" sparse-checkout init --cone
  )
  run_checked("Selecting Swift LLVM headers"
    "${GIT_EXECUTABLE}" -C "${SWIFT_LLVM_ROOT}" sparse-checkout set
    cmake llvm/include
  )
  run_checked("Fetching pinned Swift LLVM"
    "${GIT_EXECUTABLE}" -C "${SWIFT_LLVM_ROOT}" fetch --depth 1 origin
    "${SWIFT_LLVM_COMMIT}"
  )
  run_checked("Checking out pinned Swift LLVM"
    "${GIT_EXECUTABLE}" -c advice.detachedHead=false
    -C "${SWIFT_LLVM_ROOT}" checkout --detach FETCH_HEAD
  )
endif()

execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${SWIFT_LLVM_ROOT}" rev-parse HEAD
  RESULT_VARIABLE revision_result
  OUTPUT_VARIABLE swift_llvm_revision
  ERROR_VARIABLE revision_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT revision_result EQUAL 0 OR
   NOT swift_llvm_revision STREQUAL SWIFT_LLVM_COMMIT)
  message(FATAL_ERROR
    "Swift LLVM cache must be commit ${SWIFT_LLVM_COMMIT}; found "
    "${swift_llvm_revision}${revision_error}"
  )
endif()

set(SWIFT_LLVM_VERSION_FILE
  "${SWIFT_LLVM_ROOT}/cmake/Modules/LLVMVersion.cmake"
)
file(READ "${SWIFT_LLVM_VERSION_FILE}" swift_llvm_version_file)
foreach(version_part IN ITEMS
    "LLVM_VERSION_MAJOR 19"
    "LLVM_VERSION_MINOR 1"
    "LLVM_VERSION_PATCH 5")
  string(FIND "${swift_llvm_version_file}" "${version_part}" version_match)
  if(version_match EQUAL -1)
    message(FATAL_ERROR
      "Pinned Swift LLVM commit does not declare LLVM ${SWIFT_LLVM_VERSION}"
    )
  endif()
endforeach()

find_package(
  LLVM 19.1.7 EXACT REQUIRED CONFIG
  PATHS /opt/homebrew/opt/llvm@19/lib/cmake/llvm
  NO_DEFAULT_PATH
)

set(LLVM_TABLEGEN "/opt/homebrew/opt/llvm@19/bin/llvm-tblgen")
if(NOT EXISTS "${LLVM_TABLEGEN}")
  message(FATAL_ERROR "Install the table generator with: brew install llvm@19")
endif()

file(MAKE_DIRECTORY
  "${SWIFT_LLVM_GENERATED}/llvm/IR"
  "${SWIFT_LLVM_GENERATED}/llvm/CodeGen"
)
run_checked("Generating Swift LLVM attributes"
  "${LLVM_TABLEGEN}" -gen-attrs
  -I "${SWIFT_LLVM_ROOT}/llvm/include"
  "${SWIFT_LLVM_ROOT}/llvm/include/llvm/IR/Attributes.td"
  -o "${SWIFT_LLVM_GENERATED}/llvm/IR/Attributes.inc"
)
run_checked("Generating Swift LLVM intrinsic enums"
  "${LLVM_TABLEGEN}" -gen-intrinsic-enums
  -I "${SWIFT_LLVM_ROOT}/llvm/include"
  "${SWIFT_LLVM_ROOT}/llvm/include/llvm/IR/Intrinsics.td"
  -o "${SWIFT_LLVM_GENERATED}/llvm/IR/IntrinsicEnums.inc"
)
run_checked("Generating Swift LLVM value types"
  "${LLVM_TABLEGEN}" -gen-vt
  -I "${SWIFT_LLVM_ROOT}/llvm/include"
  "${SWIFT_LLVM_ROOT}/llvm/include/llvm/CodeGen/ValueTypes.td"
  -o "${SWIFT_LLVM_GENERATED}/llvm/CodeGen/GenVT.inc"
)

add_library(HotfixPass MODULE Sources/HotfixPass.cpp)
target_compile_features(HotfixPass PRIVATE cxx_std_20)
target_include_directories(
  HotfixPass SYSTEM BEFORE PRIVATE
  "${SWIFT_LLVM_GENERATED}"
  "${SWIFT_LLVM_ROOT}/llvm/include"
  ${LLVM_INCLUDE_DIRS}
)
# Xcode's frontend does not export LLVM's link-time ABI sentinel.
target_compile_definitions(
  HotfixPass PRIVATE LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1
)
add_definitions(${LLVM_DEFINITIONS})

set_target_properties(
  HotfixPass
  PROPERTIES
    PREFIX "lib"
    SUFFIX ".dylib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.build"
)

if(APPLE)
  target_link_options(HotfixPass PRIVATE "LINKER:-undefined,dynamic_lookup")
endif()
