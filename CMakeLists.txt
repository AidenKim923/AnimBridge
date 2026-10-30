cmake_minimum_required(VERSION 3.24)
if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.5" CACHE STRING "Minimum supported macOS version")
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ANIMBRIDGE_VERSION LIMIT_COUNT 1)
project(AnimBridge VERSION ${ANIMBRIDGE_VERSION} LANGUAGES C CXX)

# These fixed destinations are the only links the desktop host can open.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/native/app-config.json" ANIMBRIDGE_APP_CONFIG)
foreach(link IN ITEMS github fairy githubSponsors)
  string(JSON ANIMBRIDGE_LINK_TYPE TYPE "${ANIMBRIDGE_APP_CONFIG}" "${link}Url")
  if(NOT ANIMBRIDGE_LINK_TYPE STREQUAL "STRING")
    message(FATAL_ERROR "native/app-config.json ${link}Url must be a string")
  endif()
  string(JSON ANIMBRIDGE_${link}_URL GET "${ANIMBRIDGE_APP_CONFIG}" "${link}Url")
  if(ANIMBRIDGE_${link}_URL AND NOT ANIMBRIDGE_${link}_URL MATCHES "^https://[^/ \t\r\n\"\\\\]+([/?#][^ \t\r\n\"\\\\]*)?$")
    message(FATAL_ERROR "native/app-config.json ${link}Url must be empty or an HTTPS URL")
  endif()
endforeach()
configure_file(native/include/animbridge/app_config.hpp.in generated/animbridge/app_config.hpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION native/app-config.json)
option(ANIMBRIDGE_ENABLE_SOURCE_FALLBACK "Allow developer builds to find backend resources in the source checkout" ON)
option(ANIMBRIDGE_BUILD_DESKTOP "Build the native desktop shell" ON)
option(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS "Enable UI automation hooks (never for a public release)" OFF)
if(APPLE AND ANIMBRIDGE_BUILD_DESKTOP)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()
if(WIN32 AND ANIMBRIDGE_BUILD_DESKTOP)
  enable_language(RC)
endif()
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(zlib URL https://zlib.net/fossils/zlib-1.3.1.tar.gz
  URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(UTF8PROC_INSTALL OFF CACHE BOOL "" FORCE)
set(UTF8PROC_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(utf8proc
  URL https://codeload.github.com/JuliaStrings/utf8proc/tar.gz/refs/tags/v2.10.0
  URL_HASH SHA256=6f4f1b639daa6dca9f80bc5db1233e9cbaa31a67790887106160b33ef743f136
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json zlib utf8proc)
find_package(Threads REQUIRED)
add_library(animbridge_core native/src/core.cpp native/src/package.cpp native/src/process.cpp native/src/workflow.cpp native/src/runtime.cpp
  native/src/path_identity.cpp native/src/file_commit.cpp native/src/instance_lock.cpp)
target_include_directories(animbridge_core PUBLIC native/include "${CMAKE_CURRENT_BINARY_DIR}/generated" PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_link_libraries(animbridge_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads PRIVATE zlibstatic utf8proc)
if(UNIX AND NOT APPLE)
  find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
  target_link_libraries(animbridge_core PRIVATE OpenSSL::Crypto)
endif()
if(ANIMBRIDGE_ENABLE_SOURCE_FALLBACK)
  target_compile_definitions(animbridge_core PRIVATE ANIMBRIDGE_SOURCE_FALLBACK="${CMAKE_CURRENT_SOURCE_DIR}")
endif()
if(MSVC)
  target_compile_options(animbridge_core PUBLIC /utf-8 /W4 /EHsc)
  target_compile_definitions(animbridge_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
add_library(animbridge_authority native/src/platform_services.cpp native/src/path_grants.cpp native/src/ui_resources.cpp)
target_link_libraries(animbridge_authority PUBLIC animbridge_core)
if(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS)
  target_compile_definitions(animbridge_authority PUBLIC ANIMBRIDGE_ENABLE_UI_TEST_HOOKS=1)
endif()
add_executable(animbridge native/src/main.cpp)
set_target_properties(animbridge PROPERTIES OUTPUT_NAME AnimBridgeCli)
target_link_libraries(animbridge PRIVATE animbridge_authority)
add_executable(AnimBridgeProcessSupervisor native/src/process_supervisor_main.cpp native/src/platform/common/process_supervisor.cpp)
target_link_libraries(AnimBridgeProcessSupervisor PRIVATE animbridge_core)
if(ANIMBRIDGE_BUILD_DESKTOP)
  # Release/harness comparisons configure after freezing the input tree. Do not
  # trigger competing MSBuild reconfiguration jobs on every edited source file.
  file(GLOB_RECURSE ANIMBRIDGE_SNAPSHOT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/native/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/native/*.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/native/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/native/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/native/*.in" "${CMAKE_CURRENT_SOURCE_DIR}/native/*.manifest"
    "${CMAKE_CURRENT_SOURCE_DIR}/native/*.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/native/*.rc" "${CMAKE_CURRENT_SOURCE_DIR}/native/*.ico"
    "${CMAKE_CURRENT_SOURCE_DIR}/native/ui/*.js" "${CMAKE_CURRENT_SOURCE_DIR}/native/ui/*.mjs"
    "${CMAKE_CURRENT_SOURCE_DIR}/native/ui/*.html" "${CMAKE_CURRENT_SOURCE_DIR}/native/ui/*.css"
    "${CMAKE_CURRENT_SOURCE_DIR}/native/ui/*.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/unityanimbridge/*.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/blender/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/unity/*.cs"
    "${CMAKE_CURRENT_SOURCE_DIR}/unreal/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/assets/characters/*.fbx"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/*.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/*.ps1" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/*.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/*.json" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/*.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/*.iss" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/*.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/*.plist" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/*.entitlements"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/*.desktop"
    "${CMAKE_CURRENT_SOURCE_DIR}/.github/workflows/*.yml" "${CMAKE_CURRENT_SOURCE_DIR}/.github/workflows/*.yaml"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.mjs")
  list(APPEND ANIMBRIDGE_SNAPSHOT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_SOURCE_DIR}/CMakePresets.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" "${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml" "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  list(SORT ANIMBRIDGE_SNAPSHOT_FILES)
  set(ANIMBRIDGE_SNAPSHOT_INPUT "")
  foreach(source IN LISTS ANIMBRIDGE_SNAPSHOT_FILES)
    file(SHA256 "${source}" digest)
    file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}" "${source}")
    string(APPEND ANIMBRIDGE_SNAPSHOT_INPUT "${relative}:${digest}\n")
  endforeach()
  string(SHA256 ANIMBRIDGE_SOURCE_SNAPSHOT_SHA "${ANIMBRIDGE_SNAPSHOT_INPUT}")
  add_library(animbridge_desktop native/src/desktop_bridge.cpp)
  target_link_libraries(animbridge_desktop PUBLIC animbridge_authority)
  target_compile_definitions(animbridge_desktop PUBLIC ANIMBRIDGE_SOURCE_SNAPSHOT_SHA="${ANIMBRIDGE_SOURCE_SNAPSHOT_SHA}")
  if(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS)
    target_compile_definitions(animbridge_desktop PUBLIC ANIMBRIDGE_ENABLE_UI_TEST_HOOKS=1)
  endif()
endif()
if(WIN32)
  target_link_libraries(animbridge_core PRIVATE bcrypt shell32 ole32 user32)
endif()
if(WIN32 AND ANIMBRIDGE_BUILD_DESKTOP)
  # Official stable WebView2 SDK; the NuGet payload is pinned and verified.
  FetchContent_Declare(webview2
    URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.3650.58/microsoft.web.webview2.1.0.3650.58.nupkg
    URL_HASH SHA256=911a472128c82ac8baa0c486c23342cc9dd6e7dc50d754e676726642ca065c60
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(webview2)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ANIMBRIDGE_WEBVIEW_ARCH x64)
  else()
    set(ANIMBRIDGE_WEBVIEW_ARCH x86)
  endif()
  add_executable(AnimBridgeDesktop WIN32 native/src/winmain.cpp native/app.manifest native/resources/animbridge.rc)
  target_include_directories(AnimBridgeDesktop PRIVATE native/resources)
  set_source_files_properties(native/resources/animbridge.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/native/resources/animbridge.ico;${CMAKE_CURRENT_SOURCE_DIR}/native/resources/resource.h")
  if(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS)
    set_target_properties(AnimBridgeDesktop PROPERTIES OUTPUT_NAME AnimBridgeHarness)
  else()
    set_target_properties(AnimBridgeDesktop PROPERTIES OUTPUT_NAME AnimBridge)
  endif()
  target_include_directories(AnimBridgeDesktop PRIVATE ${webview2_SOURCE_DIR}/build/native/include)
  target_link_libraries(animbridge_desktop PRIVATE shell32 ole32 user32)
  target_link_libraries(AnimBridgeDesktop PRIVATE animbridge_desktop
    ${webview2_SOURCE_DIR}/build/native/${ANIMBRIDGE_WEBVIEW_ARCH}/WebView2LoaderStatic.lib
    shell32 ole32 oleaut32 shlwapi version user32 advapi32 bcrypt dwmapi winhttp)
  add_custom_command(TARGET AnimBridgeDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/native/ui" "$<TARGET_FILE_DIR:AnimBridgeDesktop>/ui"
    COMMENT "Copying local AnimBridge UI assets")
  # Also refresh assets on a build when only HTML/CSS/JavaScript changed.
  add_custom_target(animbridge_ui_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/native/ui" "$<TARGET_FILE_DIR:AnimBridgeDesktop>/ui"
    DEPENDS AnimBridgeDesktop)
endif()
if(APPLE AND ANIMBRIDGE_BUILD_DESKTOP)
  add_executable(AnimBridgeDesktop MACOSX_BUNDLE native/src/platform/macos/desktop_shell.mm)
  set_target_properties(AnimBridgeDesktop PROPERTIES OUTPUT_NAME AnimBridge MACOSX_BUNDLE_BUNDLE_NAME AnimBridge
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.animbridge.desktop" MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/native/app-macos.plist.in")
  if(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS)
    set_target_properties(AnimBridgeDesktop PROPERTIES OUTPUT_NAME AnimBridgeHarness MACOSX_BUNDLE_BUNDLE_NAME AnimBridgeHarness)
  endif()
  target_compile_options(AnimBridgeDesktop PRIVATE -fobjc-arc)
  target_link_libraries(AnimBridgeDesktop PRIVATE animbridge_desktop "-framework Cocoa" "-framework WebKit")
elseif(UNIX AND ANIMBRIDGE_BUILD_DESKTOP)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
  pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET "webkit2gtk-4.1>=2.40")
  add_executable(AnimBridgeDesktop native/src/platform/linux/desktop_shell.cpp)
  set_target_properties(AnimBridgeDesktop PROPERTIES OUTPUT_NAME AnimBridge)
  if(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS)
    set_target_properties(AnimBridgeDesktop PROPERTIES OUTPUT_NAME AnimBridgeHarness)
  endif()
  target_link_libraries(AnimBridgeDesktop PRIVATE animbridge_desktop PkgConfig::GTK3 PkgConfig::WEBKIT)
endif()
if(ANIMBRIDGE_BUILD_DESKTOP AND NOT WIN32)
  add_custom_command(TARGET AnimBridgeDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/native/ui"
      "$<TARGET_FILE_DIR:AnimBridgeDesktop>/ui")
endif()
include(CTest)
if(BUILD_TESTING)
  add_executable(animbridge_runtime_tests native/tests/runtime_tests.cpp)
  target_link_libraries(animbridge_runtime_tests PRIVATE animbridge_core)
  add_test(NAME animbridge_runtime_paths COMMAND animbridge_runtime_tests ${CMAKE_CURRENT_BINARY_DIR}/runtime-test-work)
  add_executable(animbridge_process_tests native/tests/process_supervisor_tests.cpp)
  target_link_libraries(animbridge_process_tests PRIVATE animbridge_core)
  add_test(NAME animbridge_process_supervisor COMMAND animbridge_process_tests ${CMAKE_CURRENT_BINARY_DIR}/process-test-work)
  add_executable(animbridge_tests native/tests/core_tests.cpp)
  target_include_directories(animbridge_tests PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
  target_link_libraries(animbridge_tests PRIVATE animbridge_core zlibstatic)
  add_test(NAME animbridge_core COMMAND animbridge_tests ${CMAKE_CURRENT_BINARY_DIR}/test-work)
  # Header-only helpers (Epic launcher list, installer language); needs only nlohmann_json.
  add_executable(animbridge_install_discovery_tests native/tests/install_discovery_tests.cpp)
  target_link_libraries(animbridge_install_discovery_tests PRIVATE animbridge_core)
  add_test(NAME animbridge_install_discovery COMMAND animbridge_install_discovery_tests)
  if(ANIMBRIDGE_BUILD_DESKTOP)
    add_executable(animbridge_desktop_tests native/tests/desktop_bridge_tests.cpp)
    target_link_libraries(animbridge_desktop_tests PRIVATE animbridge_desktop)
    add_test(NAME animbridge_desktop_bridge COMMAND animbridge_desktop_tests ${CMAKE_CURRENT_BINARY_DIR}/desktop-test-work)
    add_executable(animbridge_ui_contract_tests native/tests/ui_contract_tests.cpp)
    target_link_libraries(animbridge_ui_contract_tests PRIVATE animbridge_desktop)
    add_test(NAME animbridge_ui_contract COMMAND animbridge_ui_contract_tests ${CMAKE_CURRENT_BINARY_DIR}/ui-contract-test-work)
  endif()
endif()
foreach(client IN ITEMS animbridge AnimBridgeDesktop animbridge_tests animbridge_runtime_tests animbridge_process_tests animbridge_desktop_tests animbridge_ui_contract_tests)
  if(TARGET ${client})
    add_dependencies(${client} AnimBridgeProcessSupervisor)
  endif()
endforeach()
