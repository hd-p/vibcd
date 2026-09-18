// Entry point: parse arguments, hand off to the supervisor.
//
// The process tree this creates:
//
//   baby_monitor (supervisor)  owns shared memory, reaps and restarts workers,
//                              feeds /dev/watchdog
//     |- baby_media            VI ch0 -> VENC -> RTSP, VI ch1 -> IVS -> motion
//     +- baby_audio            AI -> AENC, plus the built-in cry detector
//
// Only the supervisor forks, so only the supervisor can reap. That is what makes
// its liveness information trustworthy.
//
// --no-fork collapses this to a single process running one pipeline, for use
// under a debugger: gdb detaches from forked children by default, so in the
// normal tree the code that fails is the code gdb cannot see.

#include <getopt.h>

#include <csignal>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/supervisor.h"
#include "base/plain_log.h"
#include "base/rk_platform.h"
#include "base/shared_records.h"

namespace {

void PrintUsage(const char* program_name) {
    printf("Baby monitor for RV1106\n\n");
    printf("Usage: %s [options]\n\n", program_name);
    printf("Video:\n");
    printf("  -w, --width <px>       sensor width (default 1920)\n");
    printf("  -h, --height <px>      sensor height (default 1080)\n");
    printf("  -b, --bitrate <kbps>   encoder bitrate (default 2048)\n");
    printf("  -e, --encoder <codec>  h264 or h265 (default h264)\n");
    printf("  -p, --port <n>         RTSP port (default 554)\n");
    printf("      --stream <WxH>     encoder resolution (defaults to sensor size)\n");
    printf("\nMotion detection:\n");
    printf("  -s, --sensitivity <n>  1 low, 2 medium, 3 high (default 2)\n");
    printf("      --detect <WxH>     IVS analysis resolution (default 640x360)\n");
    printf("      --motion-area <n>  moving area threshold in per-mille (default 20)\n");
    printf("      --no-motion        drop the IVS branch entirely, leaving\n");
    printf("                         VI ch0 -> VENC only\n");
    printf("      --iqfiles <dir>    sensor tuning files for the ISP 3A loop\n");
    printf("                         (default /etc/iqfiles; empty skips 3A)\n");
    printf("\nAudio:\n");
    printf("  -r, --rate <hz>        sample rate (default 16000)\n");
    printf("      --audio-card <hw>  capture card as hw:<card>,<device>\n");
    printf("                         (default hw:0,0; check arecord -l)\n");
    printf("      --cry-model <path> BCD model, empty to use built-in heuristics\n");
    printf("                         (default /oem/usr/share/vqefiles/"
           "rkaudio_model_sed_bcd.rknn)\n");
    printf("\nSupervision:\n");
    printf("      --watchdog <path>  watchdog device, empty string to disable\n");
    printf("                         (default /dev/watchdog)\n");
    printf("      --max-restarts <n> restart attempts per worker (default 5)\n");
    printf("\nDebugging:\n");
    printf("      --no-fork <svc>    run one service (media or audio) in this\n");
    printf("                         process instead of forking workers: no\n");
    printf("                         restarts and no watchdog, so a breakpoint\n");
    printf("                         cannot reset the board\n");
    printf("\n  -?, --help             this message\n\n");
    printf("Examples:\n");
    printf("  %s\n", program_name);
    printf("  %s -w 1920 -h 1080 -b 2048 -e h265\n", program_name);
    printf("  %s --watchdog '' --max-restarts 0     # debugging\n", program_name);
    printf("  %s --no-fork media                    # debugging one pipeline\n\n",
           program_name);
}

// Long-only options take values outside the ASCII range so they cannot collide
// with the short option characters.
enum LongOnlyOption {
    kOptionStreamResolution = 0x100,
    kOptionDetectResolution,
    kOptionMotionArea,
    kOptionNoMotion,
    kOptionIqDir,
    kOptionCryModelPath,
    kOptionAudioCard,
    kOptionWatchdogDevice,
    kOptionMaxRestarts,
    kOptionNoFork,
};

const struct option kLongOptions[] = {
    {"width", required_argument, nullptr, 'w'},
    {"height", required_argument, nullptr, 'h'},
    {"bitrate", required_argument, nullptr, 'b'},
    {"encoder", required_argument, nullptr, 'e'},
    {"port", required_argument, nullptr, 'p'},
    {"sensitivity", required_argument, nullptr, 's'},
    {"rate", required_argument, nullptr, 'r'},
    {"stream", required_argument, nullptr, kOptionStreamResolution},
    {"detect", required_argument, nullptr, kOptionDetectResolution},
    {"motion-area", required_argument, nullptr, kOptionMotionArea},
    {"no-motion", no_argument, nullptr, kOptionNoMotion},
    {"iqfiles", required_argument, nullptr, kOptionIqDir},
    {"cry-model", required_argument, nullptr, kOptionCryModelPath},
    {"audio-card", required_argument, nullptr, kOptionAudioCard},
    {"watchdog", required_argument, nullptr, kOptionWatchdogDevice},
    {"max-restarts", required_argument, nullptr, kOptionMaxRestarts},
    {"no-fork", required_argument, nullptr, kOptionNoFork},
    {"help", no_argument, nullptr, '?'},
    {nullptr, 0, nullptr, 0},
};

bool ParseResolution(const char* text, uint32_t* width, uint32_t* height) {
    unsigned parsed_width = 0;
    unsigned parsed_height = 0;
    if (sscanf(text, "%ux%u", &parsed_width, &parsed_height) != 2) {
        return false;
    }
    if (parsed_width == 0 || parsed_height == 0) {
        return false;
    }
    *width = parsed_width;
    *height = parsed_height;
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Line buffering, not the default block buffering. When stdout is a file or
    // a pipe rather than a terminal, libc buffers 4 KB before flushing, so our
    // own log lines sit in that buffer while librockit's (which writes through)
    // appear immediately. Diagnosing this pipeline from a redirected log is
    // impossible when half the story is stuck in userspace, and a crash loses
    // the buffer entirely. The SDK's own samples call setlinebuf for this.
    setlinebuf(stdout);
    setlinebuf(stderr);

    // Before fork, so every worker inherits it. A viewer that disconnects
    // mid-stream leaves librtsp writing to a closed TCP socket; the kernel
    // answers with SIGPIPE, whose default action killed the whole media worker
    // (exit status -13) on every client teardown. Ignored, the write simply
    // fails with EPIPE and librtsp drops that client, which is what its own
    // "send ... failed: Connection reset by peer" path already handles.
    signal(SIGPIPE, SIG_IGN);

    baby_monitor::SupervisorConfig config;
    config.audio.cry_model_path = "/oem/usr/share/vqefiles/rkaudio_model_sed_bcd.rknn";

    // Tracks whether --stream was given, so the encoder resolution can default
    // to the sensor resolution even when -w/-h change it.
    bool stream_resolution_specified = false;

    // Set by --no-fork: run this one service in-process rather than supervising.
    bool single_service_requested = false;
    baby_monitor::MonitoredProcess single_service =
        baby_monitor::MonitoredProcess::MEDIA;

    int option = 0;
    while ((option = getopt_long(argc, argv, "w:h:b:e:p:s:r:?", kLongOptions,
                                 nullptr)) != -1) {
        switch (option) {
            case 'w':
                config.media.sensor_width = static_cast<uint32_t>(atoi(optarg));
                break;
            case 'h':
                config.media.sensor_height = static_cast<uint32_t>(atoi(optarg));
                break;
            case 'b':
                config.media.bitrate_kbps = static_cast<uint32_t>(atoi(optarg));
                break;
            case 'e':
                if (strcmp(optarg, "h264") == 0) {
                    config.media.use_h265 = false;
                } else if (strcmp(optarg, "h265") == 0) {
                    config.media.use_h265 = true;
                } else {
                    fprintf(stderr, "Unknown encoder '%s'; expected h264 or h265\n",
                            optarg);
                    return 1;
                }
                break;
            case 'p':
                config.media.rtsp_port = atoi(optarg);
                break;
            case 's': {
                const int sensitivity = atoi(optarg);
                if (sensitivity < 1 || sensitivity > 3) {
                    fprintf(stderr, "Sensitivity must be 1, 2 or 3\n");
                    return 1;
                }
                config.media.motion_sensitivity = static_cast<uint32_t>(sensitivity);
                break;
            }
            case 'r': {
                const int sample_rate = atoi(optarg);
                if (sample_rate <= 0) {
                    fprintf(stderr, "Sample rate must be positive\n");
                    return 1;
                }
                config.audio.sample_rate = static_cast<uint32_t>(sample_rate);
                // 20ms per frame. The AIO layer only accepts certain frame
                // sizes, so this keeps the relationship to the rate intact.
                config.audio.samples_per_frame = config.audio.sample_rate / 50;
                break;
            }
            case kOptionStreamResolution:
                if (!ParseResolution(optarg, &config.media.stream_width,
                                     &config.media.stream_height)) {
                    fprintf(stderr,
                            "Could not parse --stream '%s'; expected WxH, e.g. 1280x720\n",
                            optarg);
                    return 1;
                }
                stream_resolution_specified = true;
                break;
            case kOptionDetectResolution:
                if (!ParseResolution(optarg, &config.media.detect_width,
                                     &config.media.detect_height)) {
                    fprintf(stderr,
                            "Could not parse --detect '%s'; expected WxH, e.g. 640x360\n",
                            optarg);
                    return 1;
                }
                break;
            case kOptionMotionArea:
                config.media.motion_area_threshold_permille =
                    static_cast<uint32_t>(atoi(optarg));
                break;
            case kOptionNoMotion:
                config.media.enable_motion_detection = false;
                break;
            case kOptionIqDir:
                // Empty string skips the 3A loop; see the note in Initialise().
                config.media.iq_file_dir = optarg;
                break;
            case kOptionCryModelPath:
                config.audio.cry_model_path = optarg;
                break;
            case kOptionAudioCard:
                config.audio.capture_card_name = optarg;
                break;
            case kOptionWatchdogDevice:
                config.watchdog_device = optarg;
                break;
            case kOptionMaxRestarts:
                config.max_restarts = atoi(optarg);
                break;
            case kOptionNoFork:
                if (strcmp(optarg, "media") == 0) {
                    single_service = baby_monitor::MonitoredProcess::MEDIA;
                } else if (strcmp(optarg, "audio") == 0) {
                    single_service = baby_monitor::MonitoredProcess::AUDIO;
                } else {
                    fprintf(stderr, "Unknown service '%s'; expected media or audio\n",
                            optarg);
                    return 1;
                }
                single_service_requested = true;
                break;
            case '?':
                PrintUsage(argv[0]);
                return 0;
            default:
                PrintUsage(argv[0]);
                return 1;
        }
    }

    if (!stream_resolution_specified) {
        config.media.stream_width = config.media.sensor_width;
        config.media.stream_height = config.media.sensor_height;
    }

    // The ISP scales each VI channel down from the sensor image, so neither
    // channel may ask for more than the source.
    if (config.media.stream_width > config.media.sensor_width ||
        config.media.stream_height > config.media.sensor_height) {
        fprintf(stderr, "Stream %ux%u exceeds sensor %ux%u\n", config.media.stream_width,
                config.media.stream_height, config.media.sensor_width,
                config.media.sensor_height);
        return 1;
    }
    if (config.media.detect_width > config.media.sensor_width ||
        config.media.detect_height > config.media.sensor_height) {
        fprintf(stderr, "Detect %ux%u exceeds sensor %ux%u\n", config.media.detect_width,
                config.media.detect_height, config.media.sensor_width,
                config.media.sensor_height);
        return 1;
    }

    PLAIN_LOGI("Sensor %ux%u, stream %ux%u @%u kbps %s", config.media.sensor_width,
            config.media.sensor_height, config.media.stream_width,
            config.media.stream_height, config.media.bitrate_kbps,
            config.media.use_h265 ? "H.265" : "H.264");
    PLAIN_LOGI("Motion %ux%u, sensitivity %u, threshold %u per-mille",
            config.media.detect_width, config.media.detect_height,
            config.media.motion_sensitivity,
            config.media.motion_area_threshold_permille);
    PLAIN_LOGI("Audio %u Hz x%u, %u samples per frame, card %s", config.audio.sample_rate,
            config.audio.channel_count, config.audio.samples_per_frame,
            config.audio.capture_card_name.empty()
                ? "(by index)"
                : config.audio.capture_card_name.c_str());

    if (single_service_requested) {
        return baby_monitor::RunSingleService(config, single_service);
    }

    return baby_monitor::RunSupervisor(config);
}
