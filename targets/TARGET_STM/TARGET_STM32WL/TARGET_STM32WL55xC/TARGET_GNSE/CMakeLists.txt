# Copyright (c) 2020 ARM Limited. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

add_library(mbed-gnse INTERFACE)

target_sources(mbed-gnse
    INTERFACE
        PeripheralPins.c
        GNSE_radio_driver.cpp
)

target_include_directories(mbed-gnse
    INTERFACE
        .
)

target_link_libraries(mbed-gnse INTERFACE mbed-stm32wl55xc)

