#include <atomic>
#include <string>
#include <fstream>
#include <condition_variable>

extern "C" {
#include <SDL3/SDL.h>
}

struct AudioBuffer {
    std::unique_ptr<uint8_t[]> data;
    size_t size = 0;
    size_t pos = 0;
};

static constexpr int kAudioChannels = 2;
static constexpr int kAudioFreq = 48000;
static constexpr float kAudioVolume = 1.0f; // volume ranges from 0.0 - 1.0
static constexpr int kPcmBufferSize = 2 * 1920; // n * (48000*2*2*/100), bytes per n*10ms
static constexpr SDL_AudioFormat kAudioFormatS16Le = SDL_AUDIO_S16;

static AudioBuffer buffers[2]; // using double buffering to avoid stuttering in audio playback
static int active_buffer_index{0};
static std::atomic_bool buffer_ready[2]{false, false};
static std::unique_ptr<uint8_t[]> file_buffer;
static auto mixed_buffer = std::make_unique<uint8_t[]>(kPcmBufferSize);
static std::condition_variable cv;
static std::mutex cv_mutex, buffer_change_mutex;


// This function will be automatically called by the SDL3 library approximately every 10ms
static void SDLCALL AudioStreamCB(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    std::memset(mixed_buffer.get(), 0, additional_amount);

    // notify main thread that pcm buffer is empty
    if (!buffer_ready[active_buffer_index]) {
        cv.notify_one();
        SDL_PutAudioStreamData(stream, mixed_buffer.get(), additional_amount);
        return;
    }

    auto &buffer = buffers[active_buffer_index];
    const auto remain_buffer_size = static_cast<int>(buffer.size - buffer.pos);
    const int copy_size = std::min(additional_amount, remain_buffer_size);

    // change audio volume
    SDL_MixAudio(mixed_buffer.get(), buffer.data.get() + buffer.pos, kAudioFormatS16Le, copy_size, kAudioVolume);
    //SDL_Log("Send %d bytes to audio stream, buffer_idx=%d", copy_size, active_buffer_index);

    // notify main thread that one pcm buffer is empty
    auto buffer_change_lock = std::unique_lock(buffer_change_mutex);
    buffer.pos += copy_size;
    if (buffer.pos >= buffer.size) {
        buffer_ready[active_buffer_index] = false;
        active_buffer_index = !active_buffer_index;
        cv.notify_one();
    }
    buffer_change_lock.unlock();

    // put mixed audio data to audio stream
    SDL_PutAudioStreamData(stream, mixed_buffer.get(), additional_amount);
}

static bool PlayPcmAudio(const std::string &pcm_file) {
    SDL_AudioStream *stream = nullptr;
    constexpr SDL_AudioSpec spec = {
        .format = kAudioFormatS16Le,
        .channels = kAudioChannels,
        .freq = kAudioFreq
    }; // std=c++20

    // open device in a paused state
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, AudioStreamCB, nullptr);
    if (!stream) {
        SDL_Log("Couldn't open audio device: %s", SDL_GetError());
        return false;
    }

    std::ifstream file(pcm_file, std::ios::binary);
    if (!file.is_open()) {
        SDL_Log("Couldn't open pcm file: %s", pcm_file.c_str());
        SDL_DestroyAudioStream(stream);
        return false;
    }

    file_buffer = std::make_unique<uint8_t[]>(kPcmBufferSize);
    buffers[0].data = std::make_unique<uint8_t[]>(kPcmBufferSize);
    buffers[1].data = std::make_unique<uint8_t[]>(kPcmBufferSize);

    // begin to playback audio
    SDL_ResumeAudioStreamDevice(stream);

    // read pcm data from file
    uint64_t total_bytes_read = 0;
    while (true) {
        file.read(reinterpret_cast<char *>(file_buffer.get()), kPcmBufferSize);
        const uint64_t bytes_read = file.gcount();
        if (bytes_read == 0) {
            SDL_Log("End of pcm file");
            break;
        }

        total_bytes_read += bytes_read;
        //SDL_Log("Read %lld bytes from pcm file, total=%lld", bytes_read, total_bytes_read);

        // wait until one pcm buffer is empty
        auto cv_lock = std::unique_lock(cv_mutex);
        cv.wait(cv_lock, [&]() -> bool { return !buffer_ready[0] || !buffer_ready[1]; });
        cv_lock.unlock();

        // fill pcm buffer
        auto buffer_change_lock = std::unique_lock(buffer_change_mutex);
        const int idx = active_buffer_index;
        if (!buffer_ready[idx]) {
            //SDL_Log("Fill %d bytes to pcm buffer, buffer_idx=%d", bytes_read, idx);
            std::memcpy(buffers[idx].data.get(), file_buffer.get(), bytes_read);
            buffers[idx].size = bytes_read;
            buffers[idx].pos = 0;
            buffer_ready[idx] = true;
        } else if (!buffer_ready[!idx]) {
            //SDL_Log("Fill %d bytes to pcm buffer, buffer_idx=%d", bytes_read, !idx);
            std::memcpy(buffers[!idx].data.get(), file_buffer.get(), bytes_read);
            buffers[!idx].size = bytes_read;
            buffers[!idx].pos = 0;
            buffer_ready[!idx] = true;
        }
        buffer_change_lock.unlock();
    }

    SDL_DestroyAudioStream(stream);
    file.close();
    return true;
}

int main() {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return 1;
    }

    // ffmpeg -i test.mp4 -ar 48000 -ac 2 -f s16le 48000_16bit_2ch.pcm
    const auto pcm_file = "../../../../48000_16bit_2ch.pcm";

    // play
    const bool success = PlayPcmAudio(pcm_file);

    SDL_Quit();
    return success ? 0 : 1;
}
