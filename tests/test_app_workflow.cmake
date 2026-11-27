# The app loads the default profile at startup. Never point this test at the
# developer's config, even when it is run twice or two builds run in parallel.
string(RANDOM LENGTH 12 suffix)
set(home "${CMAKE_CURRENT_BINARY_DIR}/bm-test-home-${suffix}")
file(MAKE_DIRECTORY "${home}")
foreach(var HOME APPDATA XDG_CONFIG_HOME)
    set(ENV{${var}} "${home}")
endforeach()
execute_process(COMMAND "${TEST_EXE}" "${home}" WORKING_DIRECTORY "${home}" RESULT_VARIABLE result)
file(REMOVE_RECURSE "${home}")
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "App workflow test failed: ${result}")
endif()
