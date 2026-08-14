cmake_minimum_required(VERSION 3.24)

project(HotfixPass LANGUAGES C CXX)

set(SWIFT_LLVM_COMMIT "8f0d2ca924db37c8f8161d55c21b9097b05b72f3")
set(SWIFT_LLVM_VERSION "19.1.5")
set(
  SWIFT_LLVM_REPOSITORY
  "https://github.com/swiftlang/llvm-project.git"
  CACHE STRING "Repository used to bootstrap the pinned Swift LLVM headers"
)
set(
  SWIFT_LLVM_CACHE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/.toolchain"
  CACHE PATH "Directory for pinned Swift LLVM headers and generated tables"
)
set(SWIFT_LLVM_ROOT "${SWIFT_LLVM_CACHE_DIR}/swift-llvm-project")
set(SWIFT_LLVM_GENERATED "${SWIFT_LLVM_CACHE_DIR}/generated")
include(cmake/ToolchainValidation.cmake)

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
hotfix_swift_version_matches("${swift_version}" swift_version_matches)
if(NOT swift_result EQUAL 0 OR NOT swift_version_matches)
  message(FATAL_ERROR
    "HotfixPass requires Apple Swift 6.2.4 "
    "(swiftlang-6.2.4.1.4 clang-1700.6.4.2), found:\n${swift_version}"
  )
endif()

function(validate_swift_llvm_checkout checkout_root)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${checkout_root}" rev-parse --verify HEAD
    RESULT_VARIABLE checkout_revision_result
    OUTPUT_VARIABLE checkout_revision
    ERROR_VARIABLE checkout_revision_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT checkout_revision_result EQUAL 0 OR
     NOT checkout_revision STREQUAL SWIFT_LLVM_COMMIT)
    message(FATAL_ERROR
      "Swift LLVM cache must be commit ${SWIFT_LLVM_COMMIT}; found "
      "${checkout_revision}${checkout_revision_error}"
    )
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${checkout_root}" status
      --porcelain=v1 --untracked-files=all
    RESULT_VARIABLE checkout_status_result
    OUTPUT_VARIABLE checkout_status
    ERROR_VARIABLE checkout_status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT checkout_status_result EQUAL 0)
    message(FATAL_ERROR
      "Could not inspect Swift LLVM cache:\n${checkout_status_error}"
    )
  endif()
  if(NOT checkout_status STREQUAL "")
    message(FATAL_ERROR
      "Swift LLVM cache must be pristine; found:\n${checkout_status}"
    )
  endif()

  set(version_file "${checkout_root}/cmake/Modules/LLVMVersion.cmake")
  file(READ "${version_file}" version_file_contents)
  hotfix_llvm_version_matches("${version_file_contents}" version_matches)
  if(NOT version_matches)
    message(FATAL_ERROR
      "Pinned Swift LLVM commit does not declare LLVM ${SWIFT_LLVM_VERSION}"
    )
  endif()
endfunction()

set(swift_llvm_needs_bootstrap FALSE)
if(EXISTS "${SWIFT_LLVM_ROOT}")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SWIFT_LLVM_ROOT}"
      rev-parse --verify HEAD
    RESULT_VARIABLE existing_revision_result
    OUTPUT_QUIET
    ERROR_QUIET
  )
  if(NOT existing_revision_result EQUAL 0)
    file(REMOVE_RECURSE "${SWIFT_LLVM_ROOT}")
    set(swift_llvm_needs_bootstrap TRUE)
  endif()
else()
  set(swift_llvm_needs_bootstrap TRUE)
endif()

if(swift_llvm_needs_bootstrap)
  set(bootstrap_root "${SWIFT_LLVM_ROOT}.bootstrap")
  file(REMOVE_RECURSE "${bootstrap_root}")
  file(MAKE_DIRECTORY "${SWIFT_LLVM_CACHE_DIR}")
  run_checked("Initializing Swift LLVM cache"
    "${GIT_EXECUTABLE}" init "${bootstrap_root}"
  )
  run_checked("Adding Swift LLVM remote"
    "${GIT_EXECUTABLE}" -C "${bootstrap_root}" remote add origin
    "${SWIFT_LLVM_REPOSITORY}"
  )
  run_checked("Enabling sparse checkout"
    "${GIT_EXECUTABLE}" -C "${bootstrap_root}" sparse-checkout init --cone
  )
  run_checked("Selecting Swift LLVM headers"
    "${GIT_EXECUTABLE}" -C "${bootstrap_root}" sparse-checkout set
    cmake llvm/include
  )
  run_checked("Fetching pinned Swift LLVM"
    "${GIT_EXECUTABLE}" -C "${bootstrap_root}" fetch --depth 1 origin
    "${SWIFT_LLVM_COMMIT}"
  )
  run_checked("Checking out pinned Swift LLVM"
    "${GIT_EXECUTABLE}" -c advice.detachedHead=false
    -C "${bootstrap_root}" checkout --detach FETCH_HEAD
  )
  validate_swift_llvm_checkout("${bootstrap_root}")

  file(RENAME "${bootstrap_root}" "${SWIFT_LLVM_ROOT}" RESULT rename_result)
  if(NOT rename_result STREQUAL "0")
    message(FATAL_ERROR
      "Could not activate Swift LLVM cache: ${rename_result}"
    )
  endif()
endif()

validate_swift_llvm_checkout("${SWIFT_LLVM_ROOT}")

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
