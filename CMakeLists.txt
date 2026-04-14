cmake_minimum_required(VERSION 3.10)
project(oasis-echo VERSION 1.0.0 LANGUAGES C)

# Set C standard
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# Default to Release build if not specified
if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE Release)
endif()

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)
pkg_check_modules(JSONC REQUIRED json-c)

# Include directories
include_directories(
   include
   ${MOSQUITTO_INCLUDE_DIRS}
   ${JSONC_INCLUDE_DIRS}
)

# Source files
set(SOURCES
   src/at_command.c
   src/logging.c
   src/modem.c
   src/mqtt_comms.c
   src/oasis-echo.c
   src/sms.c
   src/urc_handler.c
)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Link libraries
target_link_libraries(${PROJECT_NAME}
   ${MOSQUITTO_LIBRARIES}
   ${JSONC_LIBRARIES}
   pthread
   m
)

# Install target
install(TARGETS ${PROJECT_NAME}
   RUNTIME DESTINATION bin
)

# Unit tests (Unity framework)
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
   enable_testing()

   # Unity test framework (vendored)
   add_library(unity STATIC tests/unity/unity.c)
   target_include_directories(unity PUBLIC tests/unity)

   # Shared logging stub (tests don't need real logging)
   add_library(echo_logging STATIC src/logging.c)
   target_include_directories(echo_logging PUBLIC include)

   # test_at_command — AT response parsing (no serial port needed)
   add_executable(test_at_command tests/test_at_command.c src/at_command.c)
   target_link_libraries(test_at_command unity echo_logging pthread)
   target_include_directories(test_at_command PRIVATE include)
   add_test(NAME test_at_command COMMAND test_at_command)

   # test_sms — phone number validation, body sanitization
   add_executable(test_sms tests/test_sms.c src/sms.c)
   target_link_libraries(test_sms unity echo_logging)
   target_include_directories(test_sms PRIVATE include)
   add_test(NAME test_sms COMMAND test_sms)

   # test_urc_handler — URC classification (no serial port needed)
   add_executable(test_urc_handler tests/test_urc_handler.c src/urc_handler.c src/sms.c
                  src/at_command.c)
   target_link_libraries(test_urc_handler unity echo_logging pthread)
   target_include_directories(test_urc_handler PRIVATE include)
   add_test(NAME test_urc_handler COMMAND test_urc_handler)

   # test_mqtt_messages — JSON construction/parsing (no broker needed)
   add_executable(test_mqtt_messages tests/test_mqtt_messages.c src/mqtt_comms.c)
   target_link_libraries(test_mqtt_messages unity echo_logging ${JSONC_LIBRARIES}
                         ${MOSQUITTO_LIBRARIES})
   target_include_directories(test_mqtt_messages PRIVATE include ${JSONC_INCLUDE_DIRS})
   add_test(NAME test_mqtt_messages COMMAND test_mqtt_messages)
endif()
