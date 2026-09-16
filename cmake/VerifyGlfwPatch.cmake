include_guard(GLOBAL)

function(_avb0795_count_exact_line output lines expected)
  set(count 0)
  foreach(line IN LISTS lines)
    if(line STREQUAL expected)
      math(EXPR count "${count} + 1")
    endif()
  endforeach()
  set(${output} ${count} PARENT_SCOPE)
endfunction()

function(avb0795_verify_glfw_patch glfw_source_dir)
  set(main_class_source "${glfw_source_dir}/src/win32_window.c")
  set(helper_class_source "${glfw_source_dir}/src/win32_init.c")

  if(NOT EXISTS "${main_class_source}" OR NOT EXISTS "${helper_class_source}")
    message(FATAL_ERROR
      "The pinned GLFW 3.4 Win32 sources are missing after population")
  endif()

  file(STRINGS "${main_class_source}" main_class_lines)
  set(expected_main_line
    "        wc.style         = CS_HREDRAW | CS_VREDRAW;")
  _avb0795_count_exact_line(
    main_line_count "${main_class_lines}" "${expected_main_line}")
  if(NOT main_line_count EQUAL 1)
    message(FATAL_ERROR
      "GLFW 3.4 main Win32 class style drifted: expected exactly one patched '${expected_main_line}' line")
  endif()

  file(READ "${main_class_source}" main_class_contents)
  if(main_class_contents MATCHES
     "wc\\.style[ \t]*=[^;\r\n]*CS_OWNDC[^;\r\n]*;")
    message(FATAL_ERROR
      "GLFW 3.4 main Win32 window class still requests CS_OWNDC")
  endif()

  file(STRINGS "${helper_class_source}" helper_class_lines)
  set(expected_helper_line "    wc.style         = CS_OWNDC;")
  _avb0795_count_exact_line(
    helper_line_count "${helper_class_lines}" "${expected_helper_line}")
  if(NOT helper_line_count EQUAL 1)
    message(FATAL_ERROR
      "GLFW 3.4 helper Win32 class drifted: expected exactly one upstream '${expected_helper_line}' line")
  endif()

  message(STATUS
    "Verified avb0795's GLFW 3.4 Win32 class patch and unchanged helper class")
endfunction()
