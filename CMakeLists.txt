cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32" CACHE STRING "" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ereader)
