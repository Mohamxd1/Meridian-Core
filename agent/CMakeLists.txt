cmake_minimum_required(VERSION 3.20)
project(meridian_agent C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(MERIDIAN_SANITIZE "Build with AddressSanitizer/UBSan (Linux)" OFF)

# Every collector wraps its body in #ifdef _WIN32 ... #else ... #endif, so the glob is portable.
file(GLOB SRC src/*.c)
list(REMOVE_ITEM SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c)

find_package(CURL REQUIRED)
add_library(agent_core STATIC ${SRC})
target_include_directories(agent_core PUBLIC include)
target_link_libraries(agent_core PUBLIC CURL::libcurl)
if(WIN32)
  target_link_libraries(agent_core PUBLIC pdh iphlpapi ws2_32)
else()
  target_link_libraries(agent_core PUBLIC dl m)
  target_compile_options(agent_core PRIVATE -Wall -Wextra)
endif()

add_executable(meridian-agent src/main.c)
target_link_libraries(meridian-agent PRIVATE agent_core)

if(MERIDIAN_SANITIZE AND NOT WIN32)
  foreach(t agent_core meridian-agent)
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

enable_testing()
add_executable(test_core tests/test_core.c)
target_link_libraries(test_core PRIVATE agent_core)
if(MERIDIAN_SANITIZE AND NOT WIN32)
  target_compile_options(test_core PRIVATE -fsanitize=address,undefined)
  target_link_options(test_core PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME core COMMAND test_core)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME agent_integration
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
                   $<TARGET_FILE:meridian-agent>)
endif()
