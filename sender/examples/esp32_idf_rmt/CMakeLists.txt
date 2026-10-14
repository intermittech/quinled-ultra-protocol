# SPDX-License-Identifier: MIT
# QuinLED Ultra sender: ESP-IDF example (ESP-IDF >= 5.3), the library as a component (main/idf_component.yml).
# Build: idf.py set-target esp32 build flash monitor
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ultra_sender_example)
