include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/ToolchainValidation.cmake")

function(expect_swift text expected)
  hotfix_swift_version_matches("${text}" matches)
  if(NOT matches STREQUAL expected)
    message(FATAL_ERROR
      "Swift parser returned ${matches}, expected ${expected} for: ${text}"
    )
  endif()
endfunction()

function(expect_llvm text expected)
  hotfix_llvm_version_matches("${text}" matches)
  if(NOT matches STREQUAL expected)
    message(FATAL_ERROR
      "LLVM parser returned ${matches}, expected ${expected} for: ${text}"
    )
  endif()
endfunction()

expect_swift(
  "swift-driver version: 1.127.15\nApple Swift version 6.2.4 (swiftlang-6.2.4.1.4 clang-1700.6.4.2)\nTarget: arm64-apple-macosx26.0"
  TRUE
)
expect_swift(
  "Apple Swift version 6.2.40 (swiftlang-6.2.4.1.4 clang-1700.6.4.2)"
  FALSE
)
expect_swift(
  "Apple Swift version 6.2.4-dev (swiftlang-6.2.4.1.4 clang-1700.6.4.2)"
  FALSE
)
expect_swift(
  "Apple Swift version 6.2.4 (swiftlang-6.2.4.1.5 clang-1700.6.4.2)"
  FALSE
)

set(valid_llvm [=[
if(NOT DEFINED LLVM_VERSION_MAJOR)
  set(LLVM_VERSION_MAJOR 19)
endif()
if(NOT DEFINED LLVM_VERSION_MINOR)
  set(LLVM_VERSION_MINOR 1)
endif()
if(NOT DEFINED LLVM_VERSION_PATCH)
  set(LLVM_VERSION_PATCH 5)
endif()
]=])
expect_llvm("${valid_llvm}" TRUE)

string(REPLACE
  "set(LLVM_VERSION_PATCH 5)"
  "set(LLVM_VERSION_PATCH 50)"
  invalid_llvm
  "${valid_llvm}"
)
expect_llvm("${invalid_llvm}" FALSE)
