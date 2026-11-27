cmake_minimum_required(VERSION 3.16)
project(BiliMessenger VERSION 2.0.1 LANGUAGES C)

set(BM_RELEASE_VERSION "" CACHE STRING "Release SemVer or tag (empty uses PROJECT_VERSION)")
include(cmake/BMVersion.cmake)
bm_resolve_version("${PROJECT_VERSION}" "${BM_RELEASE_VERSION}" BM_VERSION)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BM_BUILD_TESTS "Build the unit tests" ON)

# --- CUI (git submodule) -----------------------------------------------------
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/CUI/CMakeLists.txt)
    message(FATAL_ERROR "third_party/CUI is missing. Run: git submodule update --init --recursive")
endif()
set(CUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/CUI EXCLUDE_FROM_ALL)
include(third_party/CUI/cmake/CuiApp.cmake)

# --- libcurl -----------------------------------------------------------------
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

option(BM_ENABLE_COVERAGE "Instrument the app and tests for gcovr (Debug builds)" OFF)
if(BM_ENABLE_COVERAGE)
    if(MSVC OR NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "BM_ENABLE_COVERAGE requires GCC or Clang")
    endif()
    add_compile_options(--coverage -O0)
    add_link_options(--coverage)
endif()

if(MSVC)
    # CUI expects UTF-8, including Chinese dialog labels and default filenames.
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:/utf-8>")
endif()

# --- core library: everything except the window, so the tests can link it ----
add_library(bm_core STATIC
    src/core/util.c
    src/core/version.c
    src/core/md5.c
    src/core/http.c
    src/core/diagnostics.c
    src/core/feedback.c
    src/core/jobs.c
    src/core/store.c
    src/core/sent_history.c
    src/core/bili.c
    src/core/guards.c
    third_party/cjson/cJSON.c
    third_party/qrcodegen/qrcodegen.c
)
target_include_directories(bm_core PUBLIC
    src
    third_party/cjson
    third_party/qrcodegen
)
target_compile_definitions(bm_core PUBLIC BM_VERSION="${BM_VERSION}")
target_link_libraries(bm_core PUBLIC CURL::libcurl Threads::Threads)
if(WIN32)
    target_compile_definitions(bm_core PUBLIC _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# --- UI shared by the native window and headless tests -----------------------
add_library(bm_ui STATIC
    src/ui/shell.c
    src/ui/images.c
    src/ui/fonts.c
    src/ui/common.c
    src/ui/update.c
    src/pages/summary.c
    src/pages/guard_list.c
    src/pages/template.c
    src/pages/codes.c
    src/pages/message.c
    src/pages/setting.c
    src/pages/about.c
)
target_include_directories(bm_ui PRIVATE third_party/CUI/third_party)
target_link_libraries(bm_ui PUBLIC bm_core cui::cui)

if(APPLE)
    enable_language(OBJC)
    target_sources(bm_ui PRIVATE src/ui/window_mac.m)
    set_source_files_properties(src/ui/window_mac.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    set(MACOSX_BUNDLE_ICON_FILE icon.icns)
    set(BM_ICON ${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/icon.icns)
    set_source_files_properties(${BM_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    add_executable(BiliMessenger MACOSX_BUNDLE src/main.c ${BM_ICON})
    set_target_properties(BiliMessenger PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "BiliMessenger"
        MACOSX_BUNDLE_GUI_IDENTIFIER "cn.joi-club.bilimessenger"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/assets/Info.plist.in
    )
elseif(WIN32)
    target_sources(bm_ui PRIVATE src/ui/window_win.c)
    add_executable(BiliMessenger src/main.c assets/app.rc)
    configure_file(assets/windows-installer.iss.in BiliMessenger.iss @ONLY)
else()
    target_sources(bm_ui PRIVATE src/ui/window_stub.c)
    add_executable(BiliMessenger src/main.c)
endif()

target_link_libraries(BiliMessenger PRIVATE bm_ui)
cui_app_target(BiliMessenger)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bm_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_options(bm_ui PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    target_compile_options(BiliMessenger PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    set_source_files_properties(third_party/cjson/cJSON.c third_party/qrcodegen/qrcodegen.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

# --- tests -------------------------------------------------------------------
if(BM_BUILD_TESTS)
    enable_testing()
    add_test(NAME bm_build_version
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_build_version.cmake)
    # Test assertions (including calls under test) must run in Release too.
    if(MSVC)
        add_compile_options(/UNDEBUG)
    else()
        add_compile_options(-UNDEBUG)
    endif()
    add_executable(bm_tests tests/test_core.c)
    target_link_libraries(bm_tests PRIVATE bm_core)
    add_executable(bm_feedback_tests tests/test_feedback.c)
    target_link_libraries(bm_feedback_tests PRIVATE bm_core)
    add_executable(bm_feedback_ui_tests tests/test_feedback_ui.c src/pages/about.c)
    target_link_libraries(bm_feedback_ui_tests PRIVATE bm_core cui::cui)
    add_executable(bm_update_ui_tests tests/test_update_ui.c src/ui/update.c src/ui/fonts.c src/pages/about.c)
    target_link_libraries(bm_update_ui_tests PRIVATE bm_core cui::cui)
    add_executable(bm_guard_import_ui_tests tests/test_guard_import_ui.c src/ui/fonts.c)
    target_link_libraries(bm_guard_import_ui_tests PRIVATE bm_core cui::cui)
    target_include_directories(bm_guard_import_ui_tests PRIVATE third_party/CUI/src)
    add_executable(bm_message_tests tests/test_message.c src/ui/common.c src/ui/fonts.c src/pages/codes.c)
    target_include_directories(bm_message_tests PRIVATE third_party/CUI/src)
    target_link_libraries(bm_message_tests PRIVATE bm_core cui::cui)
    add_executable(bm_app_tests tests/test_app.c)
    target_include_directories(bm_app_tests PRIVATE third_party/CUI/src)
    target_link_libraries(bm_app_tests PRIVATE bm_ui)
    add_test(NAME bm_app_workflow COMMAND ${CMAKE_COMMAND}
        -DTEST_EXE=$<TARGET_FILE:bm_app_tests> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_app_workflow.cmake)
    set_tests_properties(bm_app_workflow PROPERTIES TIMEOUT 30)
    add_executable(bm_startup_tests tests/test_startup.c)
    target_link_libraries(bm_startup_tests PRIVATE bm_ui)
    add_test(NAME bm_startup COMMAND ${CMAKE_COMMAND}
        -DTEST_EXE=$<TARGET_FILE:bm_startup_tests> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_app_workflow.cmake)
    set_tests_properties(bm_startup PROPERTIES TIMEOUT 30 ENVIRONMENT
        "BM_PAGE=1;BM_THEME=light;BM_ACCENT=2;BM_IMPORT_DIALOG=1;BM_FEEDBACK_DIALOG=preview")
    add_test(NAME bm_tests COMMAND bm_tests)
    add_test(NAME bm_feedback_tests COMMAND bm_feedback_tests)
    add_test(NAME bm_feedback_ui_tests COMMAND bm_feedback_ui_tests)
    add_test(NAME bm_update_ui_tests COMMAND bm_update_ui_tests)
    add_test(NAME bm_guard_import_ui_tests COMMAND bm_guard_import_ui_tests)
    add_test(NAME bm_message_tests COMMAND bm_message_tests)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME bm_unicode_file_paths
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_unicode_file_paths.py
                    $<TARGET_FILE:bm_guard_import_ui_tests> $<TARGET_FILE:bm_message_tests>)
        add_test(NAME bm_feedback_http
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_feedback_http.py
                    $<TARGET_FILE:bm_feedback_tests>)
        set_tests_properties(bm_feedback_http PROPERTIES TIMEOUT 20)
    endif()
endif()
