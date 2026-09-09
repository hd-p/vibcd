// Entry point: parse arguments, hand off to the supervisor.
//
// The process tree this creates:
//
//   baby_monitor (supervisor)  owns shared memory, reaps and restarts workers,
//                              feeds /dev/watchdog
//     |- baby_media            VI -> VPSS -> {VENC -> RTSP, IVS -> motion}
//     +- baby_audio            AI -> AENC, plus the built-in cry detector
//
// Only the supervisor forks, so only the supervisor can reap. That is what makes
// its liveness information trustworthy.

#include <getopt.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/supervisor.h"
#include "base/rk_platform.h"

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
    printf("\nAudio:\n");
    printf("  -r, --rate <hz>        sample rate (default 16000)\n");
    printf("      --cry-model <path> BCD model, empty to use built-in heuristics\n");
    printf("                         (default /oem/usr/share/vqefiles/"
           "rkaudio_model_sed_bcd.rknn)\n");
    printf("\nSupervision:\n");
    printf("      --watchdog <path>  watchdog device, empty string to disable\n");
    printf("                         (default /dev/watchdog)\n");
    printf("      --max-restarts <n> restart attempts per worker (default 5)\n");
    printf("\n  -?, --help             this message\n\n");
    printf("Examples:\n");
    printf("  %s\n", program_name);
    printf("  %s -w 1920 -h 1080 -b 2048 -e h265\n", program_name);
    printf("  %s --watchdog '' --max-restarts 0     # debugging\n\n", program_name);
}

// Long-only options take values outside the ASCII range so they cannot collide
// with the short option characters.
enum LongOnlyOption {
    kOptionStreamResolution = 0x100,
    kOptionDetectResolution,
    kOptionMotionArea,
    kOptionCryModelPath,
    kOptionWatchdogDevice,
    kOptionMaxRestarts,
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
    {"cry-model", required_argument, nullptr, kOptionCryModelPath},
    {"watchdog", required_argument, nullptr, kOptionWatchdogDevice},
    {"max-restarts", required_argument, nullptr, kOptionMaxRestarts},
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
    baby_monitor::SupervisorConfig config;
    config.audio.cry_model_path = "/oem/usr/share/vqefiles/rkaudio_model_sed_bcd.rknn";

    // Tracks whether --stream was given, so the encoder resolution can default
    // to the sensor resolution even when -w/-h change it.
    bool stream_resolution_specified = false;

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
            case kOptionCryModelPath:
                config.audio.cry_model_path = optarg;
                break;
            case kOptionWatchdogDevice:
                config.watchdog_device = optarg;
                break;
            case kOptionMaxRestarts:
                config.max_restarts = atoi(optarg);
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

    // VPSS scales the sensor image down into both branches, so neither output
    // may exceed the source.
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

    RK_LOGI("Sensor %ux%u, stream %ux%u @%u kbps %s", config.media.sensor_width,
            config.media.sensor_height, config.media.stream_width,
            config.media.stream_height, config.media.bitrate_kbps,
            config.media.use_h265 ? "H.265" : "H.264");
    RK_LOGI("Motion %ux%u, sensitivity %u, threshold %u per-mille",
            config.media.detect_width, config.media.detect_height,
            config.media.motion_sensitivity,
            config.media.motion_area_threshold_permille);
    RK_LOGI("Audio %u Hz x%u, %u samples per frame", config.audio.sample_rate,
            config.audio.channel_count, config.audio.samples_per_frame);

    return baby_monitor::RunSupervisor(config);
}
