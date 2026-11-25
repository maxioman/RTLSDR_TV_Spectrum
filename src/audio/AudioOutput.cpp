#include "AudioOutput.hpp"

#include <algorithm>
#include <cstring>

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <thread>
#include <atomic>
#include <chrono>
#endif

namespace RTLSDR {

#if defined(__APPLE__)

struct AudioOutput::Impl {
    static constexpr int kNumBuffers = 4;
    static constexpr int kBufferFrames = 1024;
    AudioQueueRef queue = nullptr;
    AudioQueueBufferRef buffers[kNumBuffers] = {};
    AudioOutput* owner = nullptr;

    static void audioCallback(void* userData, AudioQueueRef aq, AudioQueueBufferRef buf) {
        auto* impl = static_cast<Impl*>(userData);
        if (!impl || !impl->owner) return;

        auto* out = static_cast<float*>(buf->mAudioData);
        size_t frames = kBufferFrames;
        size_t got = impl->owner->popSamples(out, frames);
        if (got < frames) {
            std::memset(out + got, 0, (frames - got) * sizeof(float));
        }
        buf->mAudioDataByteSize = static_cast<UInt32>(frames * sizeof(float));
        AudioQueueEnqueueBuffer(aq, buf, 0, nullptr);
    }
};

AudioOutput::AudioOutput()
    : ringBuffer_(48000 * 2, 0.0f)
    , impl_(new Impl())
{
    impl_->owner = this;
}

AudioOutput::~AudioOutput() {
    close();
    delete impl_;
    impl_ = nullptr;
}

bool AudioOutput::open(uint32_t sampleRate) {
    if (isOpen_) return true;
    sampleRate_ = sampleRate;

    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = static_cast<Float64>(sampleRate);
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 32;

    OSStatus status = AudioQueueNewOutput(&fmt, Impl::audioCallback, impl_, nullptr, nullptr, 0, &impl_->queue);
    if (status != noErr || !impl_->queue) {
        impl_->queue = nullptr;
        return false;
    }

    for (int i = 0; i < Impl::kNumBuffers; ++i) {
        status = AudioQueueAllocateBuffer(impl_->queue, Impl::kBufferFrames * sizeof(float), &impl_->buffers[i]);
        if (status != noErr) {
            close();
            return false;
        }
        std::memset(impl_->buffers[i]->mAudioData, 0, Impl::kBufferFrames * sizeof(float));
        impl_->buffers[i]->mAudioDataByteSize = Impl::kBufferFrames * sizeof(float);
        AudioQueueEnqueueBuffer(impl_->queue, impl_->buffers[i], 0, nullptr);
    }

    status = AudioQueueStart(impl_->queue, nullptr);
    if (status != noErr) {
        close();
        return false;
    }

    isOpen_ = true;
    return true;
}

void AudioOutput::close() {
    if (impl_ && impl_->queue) {
        AudioQueueStop(impl_->queue, true);
        AudioQueueDispose(impl_->queue, true);
        impl_->queue = nullptr;
    }
    isOpen_ = false;
    clear();
}

#elif defined(__linux__)

struct AudioOutput::Impl {
    void* alsaLib = nullptr;
    void* pcmHandle = nullptr;
    std::atomic<bool> running{false};
    std::thread worker;

    using snd_pcm_open_t = int (*)(void**, const char*, int, int);
    using snd_pcm_close_t = int (*)(void*);
    using snd_pcm_set_params_t = int (*)(void*, int, int, unsigned int, unsigned int, int, unsigned int);
    using snd_pcm_writei_t = long (*)(void*, const void*, unsigned long);
    using snd_pcm_prepare_t = int (*)(void*);

    snd_pcm_open_t pcmOpen = nullptr;
    snd_pcm_close_t pcmClose = nullptr;
    snd_pcm_set_params_t pcmSetParams = nullptr;
    snd_pcm_writei_t pcmWritei = nullptr;
    snd_pcm_prepare_t pcmPrepare = nullptr;
};

AudioOutput::AudioOutput()
    : ringBuffer_(48000 * 2, 0.0f)
    , impl_(new Impl())
{
}

AudioOutput::~AudioOutput() {
    close();
    delete impl_;
    impl_ = nullptr;
}

bool AudioOutput::open(uint32_t sampleRate) {
    if (isOpen_) return true;
    sampleRate_ = sampleRate;

    impl_->alsaLib = dlopen("libasound.so.2", RTLD_LAZY);
    if (!impl_->alsaLib) return false;

    impl_->pcmOpen = reinterpret_cast<Impl::snd_pcm_open_t>(dlsym(impl_->alsaLib, "snd_pcm_open"));
    impl_->pcmClose = reinterpret_cast<Impl::snd_pcm_close_t>(dlsym(impl_->alsaLib, "snd_pcm_close"));
    impl_->pcmSetParams = reinterpret_cast<Impl::snd_pcm_set_params_t>(dlsym(impl_->alsaLib, "snd_pcm_set_params"));
    impl_->pcmWritei = reinterpret_cast<Impl::snd_pcm_writei_t>(dlsym(impl_->alsaLib, "snd_pcm_writei"));
    impl_->pcmPrepare = reinterpret_cast<Impl::snd_pcm_prepare_t>(dlsym(impl_->alsaLib, "snd_pcm_prepare"));

    if (!impl_->pcmOpen || !impl_->pcmClose || !impl_->pcmSetParams || !impl_->pcmWritei || !impl_->pcmPrepare) {
        close();
        return false;
    }

    // SND_PCM_STREAM_PLAYBACK = 0, SND_PCM_FORMAT_FLOAT_LE = 14, SND_PCM_ACCESS_RW_INTERLEAVED = 3
    if (impl_->pcmOpen(&impl_->pcmHandle, "default", 0, 0) < 0 || !impl_->pcmHandle) {
        close();
        return false;
    }

    if (impl_->pcmSetParams(impl_->pcmHandle, 14, 3, 1, sampleRate, 1, 50000) < 0) {
        close();
        return false;
    }

    impl_->running = true;
    isOpen_ = true;
    impl_->worker = std::thread([this]() {
        std::vector<float> buf(1024, 0.0f);
        while (impl_->running) {
            size_t got = popSamples(buf.data(), buf.size());
            if (got < buf.size()) {
                std::fill(buf.begin() + got, buf.end(), 0.0f);
            }
            long written = impl_->pcmWritei(impl_->pcmHandle, buf.data(), buf.size());
            if (written < 0) {
                impl_->pcmPrepare(impl_->pcmHandle);
            }
        }
    });

    return true;
}

void AudioOutput::close() {
    if (impl_) {
        impl_->running = false;
        if (impl_->worker.joinable()) {
            impl_->worker.join();
        }
        if (impl_->pcmHandle && impl_->pcmClose) {
            impl_->pcmClose(impl_->pcmHandle);
            impl_->pcmHandle = nullptr;
        }
        if (impl_->alsaLib) {
            dlclose(impl_->alsaLib);
            impl_->alsaLib = nullptr;
        }
    }
    isOpen_ = false;
    clear();
}

#else

struct AudioOutput::Impl {};

AudioOutput::AudioOutput() : ringBuffer_(48000, 0.0f), impl_(new Impl()) {}
AudioOutput::~AudioOutput() { delete impl_; }
bool AudioOutput::open(uint32_t) { return false; }
void AudioOutput::close() { isOpen_ = false; }

#endif

void AudioOutput::pushSamples(const float* samples, size_t count) {
    if (!samples || count == 0 || !isOpen_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < count; ++i) {
        float s = std::max(-1.0f, std::min(1.0f, samples[i]));
        ringBuffer_[writeIdx_] = s;
        writeIdx_ = (writeIdx_ + 1) % ringBuffer_.size();
        if (writeIdx_ == readIdx_) {
            readIdx_ = (readIdx_ + 1) % ringBuffer_.size();
        }
    }
}

size_t AudioOutput::popSamples(float* dest, size_t maxCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    while (count < maxCount && readIdx_ != writeIdx_) {
        dest[count++] = ringBuffer_[readIdx_];
        readIdx_ = (readIdx_ + 1) % ringBuffer_.size();
    }
    return count;
}

void AudioOutput::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    readIdx_ = 0;
    writeIdx_ = 0;
}

} // namespace RTLSDR
