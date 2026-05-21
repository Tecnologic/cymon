# cymon — ESP32-S2 monitor firmware
# PlatformIO / ESP-IDF project root CMakeLists.txt.
#
# For host-side unit tests run:
#   cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -S host

cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cymon)
