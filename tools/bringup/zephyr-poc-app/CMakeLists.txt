# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(Blinky)

target_sources(app PRIVATE
    src/main.c
    src/persp/persp.c
    src/ble/ble.c
)

# IMU / motion cancelling: Sense board only (xiao_ble/nrf52840/sense).
# On the plain board, src/motion/motion.h supplies stubs instead.
target_sources_ifdef(CONFIG_LSM6DSL app PRIVATE src/motion/motion.c)
