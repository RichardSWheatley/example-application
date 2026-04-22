# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
<<<<<<<< HEAD:samples/audio_processor/CMakeLists.txt
project(audio_processor_sample)
========
project(swapped_app)
>>>>>>>> 292f7c9 (app: add MCUboot support with sysbuild and swap trigger):app/swapped_app/CMakeLists.txt

target_sources(app PRIVATE src/main.c)
