cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(battery_sense_testing)

# Same sources as the Arduino build; hal_arduino.cpp compiles to nothing here
# (guarded by #ifdef ARDUINO).
FILE(GLOB app_sources ../src/*.cpp)
target_sources(app PRIVATE ${app_sources})
