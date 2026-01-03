set(TARGET_NAME "sdl3_play_pcm")

add_executable(${TARGET_NAME}
        sdl3_play_pcm.cc
)

target_include_directories(${TARGET_NAME} PRIVATE
        ${SDL3_DIR}/include
)

target_link_directories(${TARGET_NAME} PRIVATE
        ${SDL3_DIR}/lib/x64
)

target_link_libraries(${TARGET_NAME} PRIVATE
        SDL3
)

if (WIN32)
    file(GLOB SDL3_DLLS "${SDL3_DIR}/lib/x64/*.dll")
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDL3_DLLS} $<TARGET_FILE_DIR:${TARGET_NAME}>
            COMMENT "Copying all required DLLs to output directory"
    )
endif ()
