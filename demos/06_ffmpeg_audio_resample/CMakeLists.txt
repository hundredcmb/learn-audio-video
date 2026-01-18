set(TARGET_NAME ffmpeg_audio_resample)

add_executable(${TARGET_NAME}
        ffmpeg_audio_resample.cpp
)

target_include_directories(${TARGET_NAME} PRIVATE
        ${FFMPEG_DIR}/include
)

target_link_directories(${TARGET_NAME} PRIVATE
        ${FFMPEG_DIR}/lib
)

target_link_libraries(${TARGET_NAME} PRIVATE
        avformat.lib # 文件格式和协议库
        avcodec.lib # 编解码库, 支持第三方 codec
        avutil.lib # 核心工具库
        avdevice.lib # 输入输出设备库
        avfilter.lib # 音视频滤镜库
        swscale.lib # 图像缩放和格式转换库
        swresample.lib # 音频重采样和格式转换库
        postproc.lib # 解码后的处理库
)

if (WIN32)
    file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:${TARGET_NAME}>
            COMMENT "Copying all required DLLs to output directory"
    )
endif ()
