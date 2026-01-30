cmake_minimum_required(VERSION 3.22)
project(learn_audio_video)

set(CMAKE_CXX_STANDARD 20)

set(SDL3_DIR "${CMAKE_SOURCE_DIR}/../SDL3-3.2.4")
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/../ffmpeg-n8.0-latest-win64-gpl-shared-8.0")

add_subdirectory(demos)
