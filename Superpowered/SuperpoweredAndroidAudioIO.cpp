#include "SuperpoweredAndroidAudioIO.h"
#include <android/log.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <atomic>
#include <cassert>
#include <mutex>
#include <thread>
#include <vector>
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>

static const int NUM_BUFFERS = 2;

using Buffer = std::vector<short>;

typedef struct SuperpoweredAndroidAudioIOInternals {
#ifndef NDEBUG
    // static_assert() would be better, but since atomic::is_lock_free() is not a constexpr,
    // this is a good place to do the checks, close to the fields using this type
    SuperpoweredAndroidAudioIOInternals() {
        assert(std::atomic<bool>().is_lock_free());
    }
#endif
    // The OpenSL ES callback may run on a different thread each time but is not reentrant (by docs),
    // so the only need for these is ensuring cache consistency via fences
    // unless both input and output are enabled, both callbacks may be called concurrently
    // so a mutex is needed on those cases to avoid race conditions. (1)

    // Thread-safe by OpenSL ES docs
    SLObjectItf openSLEngine, outputMix, outputBufferQueue, inputBufferQueue;
    SLAndroidSimpleBufferQueueItf outputBufferQueueInterface, inputBufferQueueInterface;
    // (1)
    unsigned headBufIdx, tailBufIdx;
    static_assert((UINT_MAX % NUM_BUFFERS + 1) % NUM_BUFFERS == ((UINT_MAX + 1) % NUM_BUFFERS), "");
    Buffer buffers[NUM_BUFFERS];
    // No sync needed
    int silenceSamples;
    std::mutex processMutex;
    // Atomicity is enough for these. (Btw, GCC version tested lacks std::atomic_init() for bools)
    std::atomic<bool> foreground = ATOMIC_VAR_INIT(false), started = ATOMIC_VAR_INIT(false);
    // Thread-safe (set at startup and only read on processing thread(s))
    void *clientdata;
    audioProcessingCallback callback;
    short int *silence;
    int samplerate, buffersize, latencySamples;
    bool hasOutput, hasInput;
    // Input/output processing threads (need to be synced with OpenSL ES thread)
    std::thread inputThread, outputThread;
} SuperpoweredAndroidAudioIOInternals;

static void processingLoop(std::function<void(SuperpoweredAndroidAudioIOInternals*)> audioProcessFn, SuperpoweredAndroidAudioIOInternals *internals) {
    sigset_t signalMask;
    sigemptyset(&signalMask);
    sigaddset(&signalMask, SIGUSR1);
    sigaddset(&signalMask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signalMask, nullptr);

    // Rise priority as high as possible
    for (auto nice = -19; nice < 0; ++nice) {
        if (setpriority(PRIO_PROCESS, getpid(), nice) == 0) {
            break;
        }
    }

    while (true) {
        int sig;
        if (sigwait(&signalMask, &sig) == 0) {
            if (sig == SIGUSR1) {
                audioProcessFn(internals);
            } else if (sig == SIGTERM) {
                break;
            }
        }
    }
}

static void stopQueues(SuperpoweredAndroidAudioIOInternals *internals) {
    if (!internals->started) return;
    internals->started = false;
    if (internals->outputBufferQueue) {
        SLPlayItf outputPlayInterface;
        (*internals->outputBufferQueue)->GetInterface(internals->outputBufferQueue, SL_IID_PLAY, &outputPlayInterface);
        (*outputPlayInterface)->SetPlayState(outputPlayInterface, SL_PLAYSTATE_STOPPED);
        pthread_kill(internals->outputThread.native_handle(), SIGTERM);
        internals->outputThread.join();
        (*internals->outputBufferQueueInterface)->Clear(internals->outputBufferQueueInterface);
    };
    if (internals->inputBufferQueue) {
        SLRecordItf recordInterface;
        (*internals->inputBufferQueue)->GetInterface(internals->inputBufferQueue, SL_IID_RECORD, &recordInterface);
        (*recordInterface)->SetRecordState(recordInterface, SL_RECORDSTATE_STOPPED);
        pthread_kill(internals->inputThread.native_handle(), SIGTERM);
        internals->inputThread.join();
        (*internals->inputBufferQueueInterface)->Clear(internals->inputBufferQueueInterface);
    };
}

static void processInput(SuperpoweredAndroidAudioIOInternals *internals);
static void processOutput(SuperpoweredAndroidAudioIOInternals *internals);

static void startQueues(SuperpoweredAndroidAudioIOInternals *internals) {
    if (internals->started) return;
    internals->started = true;
    internals->headBufIdx = 0;
    internals->tailBufIdx = 1;
    std::atomic_thread_fence(std::memory_order_release); // Sync headBufIdx to OpenSL thread
    if (internals->inputBufferQueue) {
        internals->inputThread = std::thread([=] () {
            processingLoop(processInput, internals);
        });
        SLRecordItf recordInterface;
        (*internals->inputBufferQueue)->GetInterface(internals->inputBufferQueue, SL_IID_RECORD, &recordInterface);
        (*recordInterface)->SetRecordState(recordInterface, SL_RECORDSTATE_RECORDING);
        // TODO: Enqueue first buffer (needed for init?)
    };
    if (internals->outputBufferQueue) {
        internals->outputThread = std::thread([=] () {
            processingLoop(processOutput, internals);
        });
        SLPlayItf outputPlayInterface;
        (*internals->outputBufferQueue)->GetInterface(internals->outputBufferQueue, SL_IID_PLAY, &outputPlayInterface);
        (*outputPlayInterface)->SetPlayState(outputPlayInterface, SL_PLAYSTATE_PLAYING);
        // Clear buffers to avoid glitch after resuming
        for (auto& buffer : internals->buffers) {
            std::fill(buffer.begin(), buffer.end(), 0);
        }
        (*internals->outputBufferQueueInterface)->Enqueue(internals->outputBufferQueueInterface, internals->silence, internals->buffersize * 4);
    };
    std::atomic_thread_fence(std::memory_order_release); // Sync everything else
}

static void processInput(SuperpoweredAndroidAudioIOInternals *internals) {
    assert(false); // Not implemented
}

static void processOutput(SuperpoweredAndroidAudioIOInternals *internals) {
    if (!internals->hasInput) {
        // Process to tail
        const auto outBufIdx = internals->tailBufIdx;
        internals->tailBufIdx = (outBufIdx + 1) % NUM_BUFFERS;
        auto& outBuf = internals->buffers[outBufIdx];
        if (!internals->callback(internals->clientdata, outBuf.data(), internals->buffersize, internals->samplerate)) {
            std::fill(outBuf.begin(), outBuf.end(), 0);
        }

        std::atomic_thread_fence(std::memory_order_release);
    } else {
        assert(false); // Not implemented
    };

    if (!internals->foreground && (internals->silenceSamples > internals->samplerate)) {
        internals->silenceSamples = 0;
        stopQueues(internals);
    };
}

static void SuperpoweredAndroidAudioIO_InputCallback(SLAndroidSimpleBufferQueueItf caller, void *pContext) { // Audio input comes here.
    SuperpoweredAndroidAudioIOInternals *internals = (SuperpoweredAndroidAudioIOInternals *)pContext;
    std::atomic_thread_fence(std::memory_order_acquire); // Sync from startup
    pthread_kill(internals->inputThread.native_handle(), SIGUSR1);
}

static void SuperpoweredAndroidAudioIO_OutputCallback(SLAndroidSimpleBufferQueueItf caller, void *pContext) {
    SuperpoweredAndroidAudioIOInternals *internals = (SuperpoweredAndroidAudioIOInternals *)pContext;
    std::atomic_thread_fence(std::memory_order_acquire); // Sync from startup,processing thread and OpenSL ES threads

    // Enqueue head
    const auto outBufIdx = internals->headBufIdx;
    internals->headBufIdx = (outBufIdx + 1) % NUM_BUFFERS;
    auto& outBuf = internals->buffers[outBufIdx];
    (*internals->outputBufferQueueInterface)->Enqueue(internals->outputBufferQueueInterface, outBuf.data(), internals->buffersize * 4);

    // Signal to process another one
    pthread_kill(internals->outputThread.native_handle(), SIGUSR1);

    std::atomic_thread_fence(std::memory_order_release); // Sync to OpenSL ES threads
}

SuperpoweredAndroidAudioIO::SuperpoweredAndroidAudioIO(int samplerate, int buffersize, bool enableInput, bool enableOutput, audioProcessingCallback callback, void *clientdata, int latencySamples) {
    static const SLboolean requireds[2] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };

    internals = new SuperpoweredAndroidAudioIOInternals;
    memset(internals, 0, sizeof(SuperpoweredAndroidAudioIOInternals));
    internals->samplerate = samplerate;
    internals->buffersize = buffersize;
    internals->clientdata = clientdata;
    internals->callback = callback;
    internals->hasInput = enableInput;
    internals->hasOutput = enableOutput;
    internals->foreground = true;
    internals->started = false;
    internals->silence = (short int *)malloc(buffersize * 4);
    internals->latencySamples = latencySamples < buffersize ? buffersize : latencySamples;
    memset(internals->silence, 0, buffersize * 4);

    for (auto i = 0; i < NUM_BUFFERS; ++i) {
        internals->buffers[i].resize(buffersize * 2);
    }

    // Create the OpenSL ES engine.
    slCreateEngine(&internals->openSLEngine, 0, NULL, 0, NULL, NULL);
    (*internals->openSLEngine)->Realize(internals->openSLEngine, SL_BOOLEAN_FALSE);
    SLEngineItf openSLEngineInterface = NULL;
    (*internals->openSLEngine)->GetInterface(internals->openSLEngine, SL_IID_ENGINE, &openSLEngineInterface);
    // Create the output mix.
    (*openSLEngineInterface)->CreateOutputMix(openSLEngineInterface, &internals->outputMix, 0, NULL, NULL);
    (*internals->outputMix)->Realize(internals->outputMix, SL_BOOLEAN_FALSE);
    SLDataLocator_OutputMix outputMixLocator = { SL_DATALOCATOR_OUTPUTMIX, internals->outputMix };

    if (enableInput) { // Create the input buffer queue.
        SLDataLocator_IODevice deviceInputLocator = { SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT, SL_DEFAULTDEVICEID_AUDIOINPUT, NULL };
        SLDataSource inputSource = { &deviceInputLocator, NULL };
        SLDataLocator_AndroidSimpleBufferQueue inputLocator = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1 };
        SLDataFormat_PCM inputFormat = { SL_DATAFORMAT_PCM, 2, samplerate * 1000, SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN };
        SLDataSink inputSink = { &inputLocator, &inputFormat };
        const SLInterfaceID inputInterfaces[1] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
        (*openSLEngineInterface)->CreateAudioRecorder(openSLEngineInterface, &internals->inputBufferQueue, &inputSource, &inputSink, 1, inputInterfaces, requireds);
        (*internals->inputBufferQueue)->Realize(internals->inputBufferQueue, SL_BOOLEAN_FALSE);
    };

    if (enableOutput) { // Create the output buffer queue.
        SLDataLocator_AndroidSimpleBufferQueue outputLocator = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1 };
        SLDataFormat_PCM outputFormat = { SL_DATAFORMAT_PCM, 2, samplerate * 1000, SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN };
        SLDataSource outputSource = { &outputLocator, &outputFormat };
        const SLInterfaceID outputInterfaces[1] = { SL_IID_BUFFERQUEUE };
        SLDataSink outputSink = { &outputMixLocator, NULL };
        (*openSLEngineInterface)->CreateAudioPlayer(openSLEngineInterface, &internals->outputBufferQueue, &outputSource, &outputSink, 1, outputInterfaces, requireds);
        (*internals->outputBufferQueue)->Realize(internals->outputBufferQueue, SL_BOOLEAN_FALSE);
    };

    if (enableInput) { // Initialize the input buffer queue.
        (*internals->inputBufferQueue)->GetInterface(internals->inputBufferQueue, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &internals->inputBufferQueueInterface);
        (*internals->inputBufferQueueInterface)->RegisterCallback(internals->inputBufferQueueInterface, SuperpoweredAndroidAudioIO_InputCallback, internals);
    };

    if (enableOutput) { // Initialize the output buffer queue.
        (*internals->outputBufferQueue)->GetInterface(internals->outputBufferQueue, SL_IID_BUFFERQUEUE, &internals->outputBufferQueueInterface);
        (*internals->outputBufferQueueInterface)->RegisterCallback(internals->outputBufferQueueInterface, SuperpoweredAndroidAudioIO_OutputCallback, internals);
    };

    startQueues(internals);
}

void SuperpoweredAndroidAudioIO::onForeground() {
    internals->foreground = true;
    startQueues(internals);
}

void SuperpoweredAndroidAudioIO::onBackground() {
    internals->foreground = false;
}

void SuperpoweredAndroidAudioIO::start() {
    startQueues(internals);
}

void SuperpoweredAndroidAudioIO::stop() {
    stopQueues(internals);
}

SuperpoweredAndroidAudioIO::~SuperpoweredAndroidAudioIO() {
    stopQueues(internals);
    usleep(10000);
    if (internals->outputBufferQueue) (*internals->outputBufferQueue)->Destroy(internals->outputBufferQueue);
    if (internals->inputBufferQueue) (*internals->inputBufferQueue)->Destroy(internals->inputBufferQueue);
    (*internals->outputMix)->Destroy(internals->outputMix);
    (*internals->openSLEngine)->Destroy(internals->openSLEngine);

    free(internals->silence);
    delete internals;
}
