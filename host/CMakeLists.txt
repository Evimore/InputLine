find_package(Threads REQUIRED)

add_library(inputline_host STATIC
  src/client_store.cpp
  src/controller_backend.cpp
  src/desktop.cpp
  src/discovery.cpp
  src/link_server.cpp
  src/log.cpp
  src/net.cpp
  src/status_file.cpp
  src/steam_controller_device.cpp
  src/tray.cpp
  src/tray_badge.cpp
  src/update_check.cpp
  src/usbip_attach.cpp
  src/usbip_server.cpp
)
target_include_directories(inputline_host PUBLIC src)
target_link_libraries(inputline_host PUBLIC inputline_core Threads::Threads)
target_compile_options(inputline_host PRIVATE ${INPUTLINE_WARNINGS})
if(WIN32)
  target_link_libraries(inputline_host PUBLIC ws2_32 user32 advapi32 wtsapi32 shell32 winhttp userenv version)
  target_compile_definitions(inputline_host PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # The tray icon: libdbus-1 is loaded at run time, so only dlopen is linked.
  target_sources(inputline_host PRIVATE src/tray_linux.cpp src/dbus_lite.cpp)
  target_link_libraries(inputline_host PUBLIC ${CMAKE_DL_LIBS})
endif()

add_executable(inputline-host src/main.cpp)
if(WIN32)
  enable_language(RC)
  target_sources(inputline-host PRIVATE src/inputline-host.rc)
  # Numeric parts of the version for the file's version resource.
  string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _version_numbers "${INPUTLINE_VERSION}")
  if(NOT _version_numbers)
    set(CMAKE_MATCH_1 0)
    set(CMAKE_MATCH_2 0)
    set(CMAKE_MATCH_3 0)
  endif()
  set_source_files_properties(src/inputline-host.rc PROPERTIES COMPILE_DEFINITIONS
    "INPUTLINE_VERSION_MAJOR=${CMAKE_MATCH_1};INPUTLINE_VERSION_MINOR=${CMAKE_MATCH_2};INPUTLINE_VERSION_PATCH=${CMAKE_MATCH_3}")
endif()
target_link_libraries(inputline-host PRIVATE inputline_host)
target_compile_options(inputline-host PRIVATE ${INPUTLINE_WARNINGS})
target_compile_definitions(inputline-host PRIVATE INPUTLINE_VERSION="${INPUTLINE_VERSION}")

add_executable(inputline-sim tools/inputline_sim.cpp)
target_link_libraries(inputline-sim PRIVATE inputline_host)
target_compile_options(inputline-sim PRIVATE ${INPUTLINE_WARNINGS})

install(TARGETS inputline-host inputline-sim RUNTIME DESTINATION bin)

if(INPUTLINE_BUILD_TESTS)
  add_executable(inputline_usbip_tests tests/test_usbip_device.cpp)
  target_link_libraries(inputline_usbip_tests PRIVATE inputline_host)
  target_include_directories(inputline_usbip_tests PRIVATE tests)
  add_test(NAME inputline_usbip_tests COMMAND inputline_usbip_tests)

  add_executable(inputline_host_unit_tests tests/test_host_units.cpp)
  target_link_libraries(inputline_host_unit_tests PRIVATE inputline_host)
  target_include_directories(inputline_host_unit_tests PRIVATE tests)
  add_test(NAME inputline_host_unit_tests COMMAND inputline_host_unit_tests)

  add_executable(inputline_link_tests tests/test_link_server.cpp)
  target_link_libraries(inputline_link_tests PRIVATE inputline_host)
  target_include_directories(inputline_link_tests PRIVATE tests)
  add_test(NAME inputline_link_tests COMMAND inputline_link_tests)
endif()
