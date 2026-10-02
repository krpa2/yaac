cmake_minimum_required(VERSION 3.16)

# Shared board support (battery power latch + PWR button)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/board_power")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sound_amy)
