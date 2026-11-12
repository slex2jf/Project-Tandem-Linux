#include <aaudio/AAudio.h>
#include <android/log.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

void audio_render(int16_t* out, int frames);

namespace {
constexpr uint32_t kMixBlockFrames = 256;
constexpr uint32_t kOutputRingFrames = 4096;
constexpr uint32_t kOutputRingMask = kOutputRingFrames - 1;
constexpr uint32_t kPrefillFrames = 1024;

static AAudioStream* g_stream = nullptr;
static std::atomic<bool> g_started{false};
static std::atomic<bool> g_paused{false};
static std::atomic<bool> g_mixer_running{false};
static std::thread g_mixer_thread;

alignas(64) static std::array<int16_t, kOutputRingFrames * 2> g_output_ring{};
alignas(64) static std::atomic<uint32_t> g_read_pos{0};
alignas(64) static std::atomic<uint32_t> g_write_pos{0};
static std::atomic<uint64_t> g_callback_frames{0};
static std::atomic<uint64_t> g_callback_underrun_frames{0};

static uint32_t ring_available() {
    return g_write_pos.load(std::memory_order_acquire) -
           g_read_pos.load(std::memory_order_acquire);
}

static void ring_clear() {
    g_read_pos.store(g_write_pos.load(std::memory_order_acquire),
                     std::memory_order_release);
}

static uint32_t ring_write(const int16_t* src, uint32_t frames) {
    const uint32_t r = g_read_pos.load(std::memory_order_acquire);
    const uint32_t w = g_write_pos.load(std::memory_order_relaxed);
    const uint32_t free_frames = kOutputRingFrames - (w - r);
    const uint32_t n = frames < free_frames ? frames : free_frames;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t slot = (w + i) & kOutputRingMask;
        g_output_ring[slot * 2] = src[i * 2];
        g_output_ring[slot * 2 + 1] = src[i * 2 + 1];
    }
    g_write_pos.store(w + n, std::memory_order_release);
    return n;
}

static uint32_t ring_read(int16_t* dst, uint32_t frames) {
    const uint32_t w = g_write_pos.load(std::memory_order_acquire);
    const uint32_t r = g_read_pos.load(std::memory_order_relaxed);
    const uint32_t n = frames < (w - r) ? frames : (w - r);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t slot = (r + i) & kOutputRingMask;
        dst[i * 2] = g_output_ring[slot * 2];
        dst[i * 2 + 1] = g_output_ring[slot * 2 + 1];
    }
    g_read_pos.store(r + n, std::memory_order_release);
    return n;
}

static void mixer_loop() {
    std::array<int16_t, kMixBlockFrames * 2> block{};
    while (g_mixer_running.load(std::memory_order_acquire)) {
        if (g_paused.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (ring_available() >= kPrefillFrames) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        // All potentially blocking/allocating work stays off the AAudio callback.
        audio_render(block.data(), kMixBlockFrames);
        if (!g_paused.load(std::memory_order_acquire))
            ring_write(block.data(), kMixBlockFrames);
    }
}

static void log_stream_stats(const char* reason) {
    if (!g_stream) return;
    __android_log_print(
        ANDROID_LOG_INFO, "TANDEM-AUDIO",
        "AAudio stats %s: xrun=%d callback_frames=%llu underrun_frames=%llu ring_frames=%u",
        reason, AAudioStream_getXRunCount(g_stream),
        (unsigned long long)g_callback_frames.load(std::memory_order_relaxed),
        (unsigned long long)g_callback_underrun_frames.load(std::memory_order_relaxed),
        ring_available());
}

static aaudio_data_callback_result_t callback(
    AAudioStream*, void*, void* data, int32_t frames) {
    int16_t* out = static_cast<int16_t*>(data);
    const uint32_t requested = frames > 0 ? static_cast<uint32_t>(frames) : 0;
    g_callback_frames.fetch_add(requested, std::memory_order_relaxed);
    if (g_paused.load(std::memory_order_acquire)) {
        std::memset(out, 0, static_cast<size_t>(requested) * 2 * sizeof(int16_t));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    const uint32_t copied = ring_read(out, requested);
    if (copied < requested) {
        const uint32_t missing = requested - copied;
        std::memset(out + copied * 2, 0,
                    static_cast<size_t>(missing) * 2 * sizeof(int16_t));
        g_callback_underrun_frames.fetch_add(missing, std::memory_order_relaxed);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
} // namespace

bool audio_open() {
    if (g_started.load(std::memory_order_acquire)) return true;
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK || !builder) {
        __android_log_print(ANDROID_LOG_ERROR, "TANDEM-AUDIO", "Builder falhou: %d", result);
        return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, 48000);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, callback, nullptr);
    result = AAudioStreamBuilder_openStream(builder, &g_stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || !g_stream) {
        __android_log_print(ANDROID_LOG_ERROR, "TANDEM-AUDIO", "OpenStream falhou: %d", result);
        g_stream = nullptr;
        return false;
    }

    const int32_t rate = AAudioStream_getSampleRate(g_stream);
    const int32_t channels = AAudioStream_getChannelCount(g_stream);
    const int32_t format = AAudioStream_getFormat(g_stream);
    const int32_t burst = AAudioStream_getFramesPerBurst(g_stream);
    const int32_t capacity = AAudioStream_getBufferCapacityInFrames(g_stream);
    const int32_t requested_buffer = burst > 0 ? burst * 4 : 1024;
    const int32_t target = capacity > 0 && requested_buffer < capacity
        ? requested_buffer : capacity;
    if (target > 0) AAudioStream_setBufferSizeInFrames(g_stream, target);
    const int32_t actual_buffer = AAudioStream_getBufferSizeInFrames(g_stream);
    __android_log_print(
        ANDROID_LOG_INFO, "TANDEM-AUDIO",
        "AAudio params: rate=%d channels=%d format=%d burst=%d capacity=%d buffer=%d perf=%d sharing=%d",
        rate, channels, format, burst, capacity, actual_buffer,
        AAudioStream_getPerformanceMode(g_stream),
        AAudioStream_getSharingMode(g_stream));

    if (rate != 48000 || channels != 2 || format != AAUDIO_FORMAT_PCM_I16) {
        __android_log_print(ANDROID_LOG_ERROR, "TANDEM-AUDIO",
                            "Formato incompatível: %d Hz, %d canais, formato %d",
                            rate, channels, format);
        AAudioStream_close(g_stream);
        g_stream = nullptr;
        return false;
    }

    g_read_pos.store(0, std::memory_order_relaxed);
    g_write_pos.store(0, std::memory_order_relaxed);
    g_callback_frames.store(0, std::memory_order_relaxed);
    g_callback_underrun_frames.store(0, std::memory_order_relaxed);
    g_mixer_running.store(true, std::memory_order_release);
    g_mixer_thread = std::thread(mixer_loop);
    g_started.store(true, std::memory_order_release);

    result = g_paused.load(std::memory_order_acquire)
        ? AAUDIO_OK : AAudioStream_requestStart(g_stream);
    if (result != AAUDIO_OK) {
        g_started.store(false, std::memory_order_release);
        g_mixer_running.store(false, std::memory_order_release);
        if (g_mixer_thread.joinable()) g_mixer_thread.join();
        AAudioStream_close(g_stream);
        g_stream = nullptr;
        __android_log_print(ANDROID_LOG_ERROR, "TANDEM-AUDIO", "Start falhou: %d", result);
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, "TANDEM-AUDIO",
                        "AAudio iniciado: 48 kHz stereo PCM16, mixer thread + SPSC ring");
    return true;
}

void audio_pause() {
    g_paused.store(true, std::memory_order_release);
    if (g_stream && g_started.load(std::memory_order_acquire)) {
        const aaudio_result_t result = AAudioStream_requestPause(g_stream);
        ring_clear();
        log_stream_stats("pause");
        __android_log_print(ANDROID_LOG_INFO, "TANDEM-AUDIO", "AAudio pausado: %d", result);
    }
}

void audio_resume() {
    ring_clear();
    g_paused.store(false, std::memory_order_release);
    if (g_stream && g_started.load(std::memory_order_acquire)) {
        const aaudio_result_t result = AAudioStream_requestStart(g_stream);
        __android_log_print(ANDROID_LOG_INFO, "TANDEM-AUDIO", "AAudio retomado: %d", result);
    }
}

void audio_close() {
    g_paused.store(true, std::memory_order_release);
    if (g_stream) AAudioStream_requestStop(g_stream);
    g_mixer_running.store(false, std::memory_order_release);
    if (g_mixer_thread.joinable()) g_mixer_thread.join();
    if (g_stream) log_stream_stats("close");
    g_started.store(false, std::memory_order_release);
    if (g_stream) AAudioStream_close(g_stream);
    g_stream = nullptr;
}
