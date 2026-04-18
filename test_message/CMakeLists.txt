cmake_minimum_required(VERSION 3.16)
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/esp-ax25"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/esp-utils")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_message)
