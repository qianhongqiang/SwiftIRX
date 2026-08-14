function(hotfix_swift_version_matches version_output result_variable)
  string(REPLACE "\r\n" "\n" normalized_output "${version_output}")
  string(REPLACE "\r" "\n" normalized_output "${normalized_output}")
  string(REPLACE "\n" ";" version_lines "${normalized_output}")

  set(match_count 0)
  foreach(line IN LISTS version_lines)
    if(line MATCHES
       "^Apple Swift version ([0-9]+\\.[0-9]+\\.[0-9]+) \\(swiftlang-([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+) clang-([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)\\)$")
      math(EXPR match_count "${match_count} + 1")
      set(swift_version "${CMAKE_MATCH_1}")
      set(swift_build "${CMAKE_MATCH_2}")
      set(clang_build "${CMAKE_MATCH_3}")
    endif()
  endforeach()

  if(match_count EQUAL 1 AND
     swift_version STREQUAL "6.2.4" AND
     swift_build STREQUAL "6.2.4.1.4" AND
     clang_build STREQUAL "1700.6.4.2")
    set(${result_variable} TRUE PARENT_SCOPE)
  else()
    set(${result_variable} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(hotfix_llvm_version_matches version_file_contents result_variable)
  string(REPLACE "\r\n" "\n" normalized_contents "${version_file_contents}")
  string(REPLACE "\r" "\n" normalized_contents "${normalized_contents}")
  string(REPLACE "\n" ";" version_lines "${normalized_contents}")

  set(valid TRUE)
  foreach(line IN LISTS version_lines)
    if(line MATCHES
       "^[ \t]*set\\(LLVM_VERSION_(MAJOR|MINOR|PATCH) ([0-9]+)\\)[ \t]*$")
      set(version_part "${CMAKE_MATCH_1}")
      if(DEFINED parsed_${version_part})
        set(valid FALSE)
      endif()
      set(parsed_${version_part} "${CMAKE_MATCH_2}")
    endif()
  endforeach()

  if(NOT DEFINED parsed_MAJOR OR
     NOT DEFINED parsed_MINOR OR
     NOT DEFINED parsed_PATCH)
    set(valid FALSE)
  elseif(NOT parsed_MAJOR STREQUAL "19" OR
         NOT parsed_MINOR STREQUAL "1" OR
         NOT parsed_PATCH STREQUAL "5")
    set(valid FALSE)
  endif()

  set(${result_variable} "${valid}" PARENT_SCOPE)
endfunction()
