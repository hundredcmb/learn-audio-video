cmake_minimum_required(VERSION 3.22)
project(learn_audio_video)

set(CMAKE_CXX_STANDARD 20)

set(SDL3_DIR "${CMAKE_SOURCE_DIR}/../SDL3-3.2.4")
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/../ffmpeg-n7.1-latest-win64-gpl-shared-7.1")

add_subdirectory(demos)
