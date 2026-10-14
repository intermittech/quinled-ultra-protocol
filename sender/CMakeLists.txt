# SPDX-License-Identifier: MIT
# QuinLED Ultra sender: an ESP-IDF component (idf_component.yml), or a plain CMake library elsewhere.
# Every source and header is in src/ (also the protocol header, a byte-identical copy of include/ultra_protocol.h).
set(ULTRA_SENDER_SRCS "src/ultra_sender.c" "src/ultra_sender_bits.c" "src/ultra_sender_rc.c")

if(ESP_PLATFORM)
    # ultra_esp32_rmt.h (header-only) uses the RMT driver's simple encoder: ESP-IDF 5.3 and later
    set(ULTRA_SENDER_REQUIRES "")
    if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
        list(APPEND ULTRA_SENDER_REQUIRES esp_driver_rmt)
    endif()
    idf_component_register(SRCS ${ULTRA_SENDER_SRCS}
                           INCLUDE_DIRS "src"
                           REQUIRES ${ULTRA_SENDER_REQUIRES})
else()
    cmake_minimum_required(VERSION 3.13)
    project(ultra_sender C)
    add_library(ultra_sender STATIC ${ULTRA_SENDER_SRCS})
    target_include_directories(ultra_sender PUBLIC src)
    set_target_properties(ultra_sender PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
endif()
