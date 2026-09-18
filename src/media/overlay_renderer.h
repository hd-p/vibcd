// On-screen overlay for the encoded stream: motion rectangles and a cry
// indicator, drawn by the encoder hardware's region (RGN) unit so the CPU
// never touches a video frame.
//
// One full-frame OVERLAY region in RK_FMT_2BPP: two bits per pixel, so a
// 1080p canvas is 518 KB and clearing plus a few rectangles costs well under a
// millisecond. Pixel value 0 is transparent; the two colour-table slots are
// motion (index 0) and cry (index 1), matching how rkipc draws its NN boxes.
//
// The renderer is a *consumer* of the event mailboxes in shared memory, exactly
// like an external process would be: it copies the latest MotionEvent and
// CryEvent under the robust mutex, then draws from its private copies. It does
// not talk to the IVS or audio code directly, so adding it changed neither.

#ifndef BABY_MONITOR_MEDIA_OVERLAY_RENDERER_H
#define BABY_MONITOR_MEDIA_OVERLAY_RENDERER_H

#include <cstdint>

#include "base/rk_platform.h"
#include "base/shared_records.h"

namespace baby_monitor {

class OverlayRenderer {
public:
    OverlayRenderer() = default;
    ~OverlayRenderer();

    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    // Creates the region and attaches it to the given VENC channel. The
    // encoder channel must already exist. Width and height are the stream
    // resolution; the canvas is rounded up to 16 as the hardware requires.
    bool Attach(int venc_channel, uint32_t width, uint32_t height);

    // Detaches and destroys the region. Must run before the VENC channel is
    // destroyed, or RGN reports the channel busy. Safe to call twice.
    void Detach();

    // Redraws the whole canvas from one motion verdict and one cry verdict.
    // A null motion event, or one with motion_present false, draws no boxes.
    bool Draw(const MotionEvent* motion, bool baby_crying);

    bool attached() const { return attached_; }

private:
    // Fills rows [y, y+height) of columns [x, x+width) with a 2-bit value.
    // Coordinates are clipped to the canvas.
    void FillRect(uint8_t* canvas, uint32_t stride_pixels, uint32_t canvas_height,
                  int32_t x, int32_t y, uint32_t width, uint32_t height,
                  uint8_t value);

    // Hollow rectangle built from four FillRect calls.
    void DrawBox(uint8_t* canvas, uint32_t stride_pixels, uint32_t canvas_height,
                 const MotionRectangle& rectangle, uint8_t value);

    static constexpr RGN_HANDLE kRegionHandle = 0;

    // Pixel values in RK_FMT_2BPP. Measured on the board, not documented:
    // value 3 shows u32ColorLUT[1], value 2 shows u32ColorLUT[0], and 0 is
    // transparent. (rkipc's draw_rect_2bpp names these the other way round,
    // which is why the first build drew the cry mark in the box colour.)
    static constexpr uint8_t kPixelMotion = 3;
    static constexpr uint8_t kPixelCry = 2;

    static constexpr uint32_t kBoxLineWidth = 4;

    // Cry indicator: a solid square in the top-left corner.
    static constexpr int32_t kCryMarkX = 16;
    static constexpr int32_t kCryMarkY = 16;
    static constexpr uint32_t kCryMarkSize = 48;

    bool created_ = false;
    bool attached_ = false;
    MPP_CHN_S venc_channel_{};
    uint32_t canvas_width_ = 0;
    uint32_t canvas_height_ = 0;
};

}  // namespace baby_monitor

#endif  // BABY_MONITOR_MEDIA_OVERLAY_RENDERER_H
