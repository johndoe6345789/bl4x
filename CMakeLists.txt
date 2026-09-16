cmake_minimum_required(VERSION 3.25)
project(bl4x CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc /MP /Zi)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  add_link_options(/DEBUG)
endif()

file(GLOB_RECURSE BL4X_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER BL4X_SOURCES EXCLUDE REGEX ".*/main\.cpp$")
add_library(bl4core STATIC ${BL4X_SOURCES})
target_include_directories(bl4core PUBLIC src)

add_executable(bl4x src/main.cpp)
target_link_libraries(bl4x PRIVATE bl4core)
