#include <string>
#include <thread>
#include <atomic>
#include <fstream>

extern "C" {
#include <SDL3/SDL.h>
}

static constexpr int kVideoWidth = 640;
static constexpr int kVideoHeight = 360;
static constexpr uint64_t kTargetFrameTimeMs = 40; // 25 fps
static constexpr SDL_PixelFormat kPixelFormatYUV420p = SDL_PIXELFORMAT_IYUV;
static constexpr auto kUserFrameEvent = static_cast<uint32_t>(SDL_EVENT_USER + 1);
static constexpr auto kUserQuitEvent = static_cast<uint32_t>(SDL_EVENT_USER + 2);

// RAII
class SDLEntity {
public:
    SDLEntity() : window_(nullptr), renderer_(nullptr), texture_(nullptr) {
    }

    void SetWindow(SDL_Window *window) { window_ = window; }

    void SetRenderer(SDL_Renderer *renderer) { renderer_ = renderer; }

    void SetTexture(SDL_Texture *texture) { texture_ = texture; }

    ~SDLEntity() {
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
    }

private:
    SDL_Window *window_;
    SDL_Renderer *renderer_;
    SDL_Texture *texture_;
};

static bool PlayYuvVideo(const std::string_view yuv_file) {
    SDLEntity entity;
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    SDL_Event event;
    SDL_FRect rect;
    int window_width = kVideoWidth;
    int window_height = kVideoHeight;
    std::atomic_bool thread_quit{false}, poll_quit{false};

    // create window and renderer
    bool success = SDL_CreateWindowAndRenderer(
        ("YUV420p Player " + std::to_string(kVideoWidth) + "x" + std::to_string(kVideoHeight)).c_str(),
        window_width,
        window_height,
        (SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE),
        &window,
        &renderer
    );
    entity.SetWindow(window);
    entity.SetRenderer(renderer);
    if (!success) {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        return false;
    }

    // create yuv texture
    texture = SDL_CreateTexture(renderer, kPixelFormatYUV420p, SDL_TEXTUREACCESS_STREAMING, kVideoWidth, kVideoHeight);
    if (!texture) {
        SDL_Log("Couldn't create texture: %s", SDL_GetError());
        return false;
    }
    entity.SetTexture(texture);

    // allocate yuv frame buffer
    constexpr int y_frame_len = kVideoWidth * kVideoHeight;
    constexpr int u_frame_len = kVideoWidth * kVideoHeight / 4;
    constexpr int v_frame_len = kVideoWidth * kVideoHeight / 4;
    constexpr int yuv_frame_len = y_frame_len + u_frame_len + v_frame_len;
    auto yuv_frame_buffer = std::make_unique<uint8_t[]>(yuv_frame_len);

    // open yuv file
    std::ifstream file(yuv_file.data(), std::ios::binary);
    if (!file.is_open()) {
        SDL_Log("Couldn't open file: %s", yuv_file.data());
        return false;
    }

    // fps control thread
    auto refresh_timer_thread = std::thread([&thread_quit]() -> void {
        while (!thread_quit.load()) {
            SDL_Event frame_event;
            frame_event.type = kUserFrameEvent;
            SDL_PushEvent(&frame_event);
            SDL_Delay(kTargetFrameTimeMs);
        }
        SDL_Event quit_event;
        quit_event.type = kUserQuitEvent;
        SDL_PushEvent(&quit_event);
    });

    // event loop
    while (!poll_quit.load()) {
        SDL_WaitEvent(&event);
        switch (event.type) {
            case SDL_EVENT_QUIT:
                thread_quit.store(true);
                continue;
            case SDL_EVENT_WINDOW_RESIZED:
                SDL_GetWindowSize(window, &window_width, &window_height);
                continue;
            case kUserQuitEvent:
                poll_quit.store(true);
                continue;
            case kUserFrameEvent:
                break;
            default:
                continue;
        }

        if (thread_quit.load()) {
            continue;
        }

        // read a yuv frame from file
        file.read(reinterpret_cast<char *>(yuv_frame_buffer.get()), yuv_frame_len);
        if (const uint64_t bytes_read = file.gcount(); bytes_read < yuv_frame_len) {
            if (bytes_read == 0) {
                SDL_Log("End of file reached. Stopping playback.");
            } else {
                SDL_Log("Couldn't read a complete frame. Stopping playback.");
            }
            thread_quit.store(true);
            continue;
        }

        // update dst rect
        constexpr float aspect_ratio = static_cast<float>(kVideoWidth) / kVideoHeight;
        const auto target_video_width1 = static_cast<float>(window_width);
        const auto target_video_height1 = static_cast<float>(window_height);
        const float target_video_width2 = target_video_height1 * aspect_ratio;
        const float target_video_height2 = target_video_width1 / aspect_ratio;
        rect.w = std::min(target_video_width1, target_video_width2);
        rect.h = std::min(target_video_height1, target_video_height2);
        rect.x = (static_cast<float>(window_width) - rect.w) / 2;
        rect.y = (static_cast<float>(window_height) - rect.h) / 2;

        // render texture
        SDL_UpdateTexture(texture, nullptr, yuv_frame_buffer.get(), kVideoWidth);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, &rect);
        SDL_RenderPresent(renderer);
    }

    file.close();

    if (refresh_timer_thread.joinable()) {
        refresh_timer_thread.join();
    }
    return true;
}


int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return 1;
    }

    // ffmpeg -i 640x360_25fps.mp4 -pix_fmt yuv420p yuv420p_640x360_25fps.yuv
    const auto yuv_file = "../../../../yuv420p_640x360_25fps.yuv";

    // play
    const bool success = PlayYuvVideo(yuv_file);

    SDL_Quit();
    return success ? 0 : 1;
}
