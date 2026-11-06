cmake_minimum_required(VERSION 3.24)
project(jooanipc_open VERSION 0.1.0 LANGUAGES C)
include(GNUInstallDirs)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
include(FetchContent)
find_package(Threads REQUIRED)
option(JIPC_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(JIPC_BUILD_TESTS "Build workstation/target unit tests" ON)
set(JIPC_MBEDTLS_ARCHIVE "" CACHE FILEPATH "Optional already-downloaded MbedTLS3.6.7 release archive")
if(JIPC_MBEDTLS_ARCHIVE)
  set(mbedtls_url "${JIPC_MBEDTLS_ARCHIVE}")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/build/downloads/mbedtls-3.6.7.tar.bz2")
  set(mbedtls_url "${CMAKE_SOURCE_DIR}/build/downloads/mbedtls-3.6.7.tar.bz2")
else()
  set(mbedtls_url "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2")
endif()
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/include/mbedtls_user_config.h" CACHE STRING "" FORCE)
FetchContent_Declare(mbedtls URL "${mbedtls_url}"
  URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(mbedtls)
# Embed the libraries without installing their development headers/archives.
# Directory properties keep this compatible with CMake 3.24 as well.
set_property(DIRECTORY "${mbedtls_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
add_library(jipc_json STATIC vendor/cjson/cJSON.c)
target_include_directories(jipc_json PUBLIC vendor/cjson)
target_compile_definitions(jipc_json PRIVATE CJSON_NESTING_LIMIT=32)
add_library(jipc_core STATIC src/util.c src/authlimit.c src/audio.c src/speaker.c src/crypto.c src/config.c src/media.c src/imp_backend.c src/watchdog.c
  src/platform.c src/aim.c src/overlay.c src/siren.c src/controls.c src/rtsp.c src/http.c src/joan.c src/onvif.c src/discovery.c src/ntp.c src/record.c
  ${CMAKE_CURRENT_BINARY_DIR}/ui_assets.h)
target_include_directories(jipc_core PUBLIC include)
target_include_directories(jipc_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
# The admin UI (media/ui) is compiled into the daemon and served from memory.
file(GLOB JIPC_UI_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ui/*.html
  ${CMAKE_CURRENT_SOURCE_DIR}/ui/*.css ${CMAKE_CURRENT_SOURCE_DIR}/ui/*.js ${CMAKE_CURRENT_SOURCE_DIR}/ui/*.svg)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ui_assets.h
  COMMAND ${CMAKE_COMMAND} -DUI_DIR=${CMAKE_CURRENT_SOURCE_DIR}/ui -DOUT=${CMAKE_CURRENT_BINARY_DIR}/ui_assets.h
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-ui.cmake
  DEPENDS ${JIPC_UI_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-ui.cmake
  COMMENT "Embedding the admin UI" VERBATIM)
target_compile_definitions(jipc_core PUBLIC JIPC_WITH_MBEDTLS _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE _LARGEFILE64_SOURCE)
target_compile_options(jipc_core PRIVATE -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
target_link_libraries(jipc_core PUBLIC jipc_json mbedtls mbedx509 mbedcrypto Threads::Threads ${CMAKE_DL_LIBS} m rt)
add_executable(jooanipc-open src/main.c)
target_compile_options(jooanipc-open PRIVATE -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
target_link_options(jooanipc-open PRIVATE -Wl,--gc-sections -Wl,--export-dynamic)
target_link_libraries(jooanipc-open PRIVATE jipc_core)
if(JIPC_SANITIZERS)
  foreach(target jipc_core jipc_json jooanipc-open)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PUBLIC -fsanitize=address,undefined)
  endforeach()
endif()
if(JIPC_BUILD_TESTS)
  enable_testing()
  add_executable(udp-probe tests/udp_probe.c)
  target_compile_options(udp-probe PRIVATE -Wall -Wextra -Werror)
  if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_core.c")
    add_executable(test-core tests/test_core.c)
    target_compile_options(test-core PRIVATE -UNDEBUG)
    target_link_libraries(test-core PRIVATE jipc_core)
    add_test(NAME core COMMAND test-core)
    add_executable(test-authlimit tests/test_authlimit.c)
    target_compile_options(test-authlimit PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_link_libraries(test-authlimit PRIVATE jipc_core)
    add_test(NAME auth-throttle COMMAND test-authlimit)
    add_executable(test-speaker tests/test_speaker.c)
    target_compile_options(test-speaker PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_link_libraries(test-speaker PRIVATE jipc_core)
    add_test(NAME speaker-service COMMAND test-speaker)
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_imp.c")
    add_executable(test-imp tests/test_imp.c)
    target_compile_options(test-imp PRIVATE -UNDEBUG)
    target_link_libraries(test-imp PRIVATE jipc_core)
    add_test(NAME imp-contract COMMAND test-imp)
  endif()
  add_executable(test-platform-motor tests/test_platform_motor.c)
  target_compile_options(test-platform-motor PRIVATE -UNDEBUG)
  target_link_options(test-platform-motor PRIVATE -Wl,--wrap=ioctl)
  target_link_libraries(test-platform-motor PRIVATE jipc_core)
  add_test(NAME native-motor COMMAND test-platform-motor)
  add_executable(test-platform-presets tests/test_platform_presets.c)
  target_compile_options(test-platform-presets PRIVATE -UNDEBUG)
  target_link_libraries(test-platform-presets PRIVATE jipc_core)
  add_test(NAME named-presets COMMAND test-platform-presets)
  add_executable(test-watchdog tests/test_watchdog.c src/watchdog.c)
  target_compile_definitions(test-watchdog PRIVATE JIPC_TEST_WATCHDOG=1)
  target_compile_options(test-watchdog PRIVATE -UNDEBUG)
  target_link_options(test-watchdog PRIVATE -Wl,--wrap=open,--wrap=open64,--wrap=close,--wrap=write,--wrap=ioctl)
  target_link_libraries(test-watchdog PRIVATE jipc_core)
  add_test(NAME watchdog COMMAND test-watchdog)
  add_executable(test-siren tests/test_siren.c)
  target_compile_options(test-siren PRIVATE -UNDEBUG)
  target_link_libraries(test-siren PRIVATE jipc_core)
  add_test(NAME siren COMMAND test-siren)
  add_executable(test-overlay tests/test_overlay.c)
  target_compile_options(test-overlay PRIVATE -UNDEBUG)
  target_link_libraries(test-overlay PRIVATE jipc_core)
  add_test(NAME overlay COMMAND test-overlay)
  add_executable(test-aim tests/test_aim.c)
  target_compile_options(test-aim PRIVATE -UNDEBUG)
  target_link_libraries(test-aim PRIVATE jipc_core)
  add_test(NAME aim COMMAND test-aim)
  add_executable(test-media-control tests/test_media_control.c)
  target_compile_options(test-media-control PRIVATE -UNDEBUG)
  target_link_libraries(test-media-control PRIVATE jipc_core)
  add_test(NAME media-control COMMAND test-media-control)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME boot-calibration COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/test_boot_calibration.py
    ${CMAKE_SOURCE_DIR}/scripts/oem-boot-handoff.sh)
  add_test(NAME boot-release-storage COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/test_boot_release_storage.py
    ${CMAKE_SOURCE_DIR}/scripts/oem-boot-handoff.sh)
  add_test(NAME native-boot COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/test_native_boot.py
    ${CMAKE_SOURCE_DIR}/scripts/native-boot.sh ${CMAKE_SOURCE_DIR}/src/main.c)
  add_test(NAME install-preflight COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/test_install_camera.py
    ${CMAKE_SOURCE_DIR}/scripts/install-camera.py)
  add_test(NAME appfs-upgrade COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/test_upgrade_appfs.py
    ${CMAKE_SOURCE_DIR}/scripts/upgrade-appfs.sh)
endif()
install(TARGETS jooanipc-open RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/replay.example.ini config/ja-a12.example.ini
  DESTINATION ${CMAKE_INSTALL_DATADIR}/jooanipc-open/examples)
install(FILES README.md dependencies.lock.json DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES docs/hardware.md docs/platform.md docs/third-party.md
  docs/verification.json docs/verification-initial-20261017.json
  docs/camera-test-20261017.json docs/camera-openimp-test-20261017.json
  docs/camera-ivdc-test-20261017.json
  docs/camera-native-test-20261017.json docs/camera-quality-test-20261017.json
  docs/camera-1080-test-20261018.json
  docs/camera-internal-flash-test-20261020.json docs/camera-audio-test-20261105.json
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
install(FILES config/replay.example.ini config/ja-a12.example.ini
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/config)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/jooanipc-open)
install(FILES vendor/cjson/LICENSE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/jooanipc-open/cjson)
install(FILES vendor/licenses/mbedtls-LICENSE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/jooanipc-open/mbedtls)
