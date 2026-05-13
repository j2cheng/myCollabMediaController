cmake_minimum_required(VERSION 3.14)
project(unit_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(unit_test
  unit_test.cpp
)

target_include_directories(unit_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../mock
)

target_link_libraries(unit_test PRIVATE
  mediacontroller
)
