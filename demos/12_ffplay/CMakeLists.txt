set(TARGET_NAME ffplay8)

add_executable(${TARGET_NAME}
        ffplay.c
        ffplay_renderer.h
        ffplay_renderer.c
        cmdutils.h
        cmdutils.c
        opt_common.h
        opt_common.c
)

target_include_directories(${TARGET_NAME} PRIVATE
        ${FFMPEG_SOURCE_DIR}
        #${FFMPEG_DIR}/include
        ${SDL2_DIR}/include
)

target_link_directories(${TARGET_NAME} PRIVATE
        ${FFMPEG_DIR}/lib
        ${SDL2_DIR}/lib/x64
)

target_link_libraries(${TARGET_NAME} PRIVATE
        avformat.lib # 文件格式和协议库
        avcodec.lib # 编解码库, 支持第三方 codec
        avutil.lib # 核心工具库
        avdevice.lib # 输入输出设备库
        avfilter.lib # 音视频滤镜库
        swscale.lib # 图像缩放和格式转换库
        swresample.lib # 音频重采样和格式转换库
        SDL2
)

if (WIN32)
    file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
    file(GLOB SDL2_DLLS "${SDL2_DIR}/lib/x64/*.dll")
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:${TARGET_NAME}>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDL2_DLLS} $<TARGET_FILE_DIR:${TARGET_NAME}>
            COMMENT "Copying all required DLLs to output directory"
    )
endif ()
