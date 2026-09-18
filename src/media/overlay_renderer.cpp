#include "media/overlay_renderer.h"

#include <cstring>

namespace baby_monitor {
namespace {

uint32_t AlignUpTo16(uint32_t value) { return (value + 15) & ~15u; }

// Colours are plain RGB888 (0xRRGGBB), verified on the board: 0x0000FF came
// out blue. rkipc's RED_COLOR/BLUE_COLOR macros are simply misnamed.
constexpr uint32_t kColourMotionBox = 0x00E0FF;  // cyan, visible on skin and bedding
constexpr uint32_t kColourCryMark = 0xFF0000;    // red

}  // namespace

OverlayRenderer::~OverlayRenderer() { Detach(); }

bool OverlayRenderer::Attach(int venc_channel, uint32_t width, uint32_t height) {
    if (attached_) {
        return true;
    }

    canvas_width_ = AlignUpTo16(width);
    canvas_height_ = AlignUpTo16(height);

    RGN_ATTR_S region;
    memset(&region, 0, sizeof(region));
    region.enType = OVERLAY_RGN;
    region.unAttr.stOverlay.enPixelFmt = RK_FMT_2BPP;
    region.unAttr.stOverlay.stSize.u32Width = canvas_width_;
    region.unAttr.stOverlay.stSize.u32Height = canvas_height_;
    // One canvas, not the default two. We redraw the entire canvas each time
    // and UpdateCanvas is synchronous, so double buffering buys nothing and
    // would double the DMA memory.
    region.unAttr.stOverlay.u32CanvasNum = 1;
    region.unAttr.stOverlay.u32ClutNum = 0;

    RK_S32 result = RK_MPI_RGN_Create(kRegionHandle, &region);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_RGN_Create failed: %#x", result);
        return false;
    }
    created_ = true;

    venc_channel_.enModId = RK_ID_VENC;
    venc_channel_.s32DevId = 0;
    venc_channel_.s32ChnId = venc_channel;

    RGN_CHN_ATTR_S display;
    memset(&display, 0, sizeof(display));
    display.bShow = RK_TRUE;
    display.enType = OVERLAY_RGN;
    display.unChnAttr.stOverlayChn.stPoint.s32X = 0;
    display.unChnAttr.stOverlayChn.stPoint.s32Y = 0;
    // Foreground fully opaque, background fully transparent: pixel value 0
    // shows the video through, the two colour slots paint over it.
    display.unChnAttr.stOverlayChn.u32BgAlpha = 0;
    display.unChnAttr.stOverlayChn.u32FgAlpha = 255;
    display.unChnAttr.stOverlayChn.u32Layer = 0;
    // Slot assignment verified on the board, see kPixelMotion / kPixelCry.
    display.unChnAttr.stOverlayChn.u32ColorLUT[0] = kColourCryMark;
    display.unChnAttr.stOverlayChn.u32ColorLUT[1] = kColourMotionBox;
    display.unChnAttr.stOverlayChn.stQpInfo.bEnable = RK_FALSE;

    result = RK_MPI_RGN_AttachToChn(kRegionHandle, &venc_channel_, &display);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_RGN_AttachToChn(VENC %d) failed: %#x", venc_channel, result);
        RK_MPI_RGN_Destroy(kRegionHandle);
        created_ = false;
        return false;
    }
    attached_ = true;

    // Start from a clean, fully transparent canvas rather than whatever the
    // allocator handed us.
    Draw(nullptr, false);

    RK_LOGI("Overlay %ux%u attached to VENC %d", canvas_width_, canvas_height_,
            venc_channel);
    return true;
}

void OverlayRenderer::Detach() {
    if (attached_) {
        RK_MPI_RGN_DetachFromChn(kRegionHandle, &venc_channel_);
        attached_ = false;
    }
    if (created_) {
        RK_MPI_RGN_Destroy(kRegionHandle);
        created_ = false;
    }
}

bool OverlayRenderer::Draw(const MotionEvent* motion, bool baby_crying) {
    if (!attached_) {
        return false;
    }

    RGN_CANVAS_INFO_S canvas_info;
    memset(&canvas_info, 0, sizeof(canvas_info));
    RK_S32 result = RK_MPI_RGN_GetCanvasInfo(kRegionHandle, &canvas_info);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_RGN_GetCanvasInfo failed: %#x", result);
        return false;
    }

    uint8_t* canvas = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(canvas_info.u64VirAddr));
    const uint32_t stride_pixels = canvas_info.u32VirWidth;
    const uint32_t canvas_height = canvas_info.u32VirHeight;
    if (canvas == nullptr || stride_pixels == 0 || canvas_height == 0) {
        RK_LOGE("Overlay canvas is empty (%p, %ux%u)", static_cast<void*>(canvas),
                stride_pixels, canvas_height);
        return false;
    }

    // Four pixels per byte. Clear to transparent, then paint.
    memset(canvas, 0, (static_cast<size_t>(stride_pixels) * canvas_height) / 4);

    if (motion != nullptr && motion->motion_present) {
        const uint32_t count = motion->rectangle_count < kMaxMotionRectangles
                                   ? motion->rectangle_count
                                   : kMaxMotionRectangles;
        for (uint32_t index = 0; index < count; ++index) {
            DrawBox(canvas, stride_pixels, canvas_height, motion->rectangles[index],
                    kPixelMotion);
        }
    }

    if (baby_crying) {
        FillRect(canvas, stride_pixels, canvas_height, kCryMarkX, kCryMarkY,
                 kCryMarkSize, kCryMarkSize, kPixelCry);
    }

    result = RK_MPI_RGN_UpdateCanvas(kRegionHandle);
    if (result != RK_SUCCESS) {
        RK_LOGE("RK_MPI_RGN_UpdateCanvas failed: %#x", result);
        return false;
    }
    return true;
}

void OverlayRenderer::FillRect(uint8_t* canvas, uint32_t stride_pixels,
                               uint32_t canvas_height, int32_t x, int32_t y,
                               uint32_t width, uint32_t height, uint8_t value) {
    // Clip to the canvas. Everything below works in whole bytes (4 pixels), so
    // x and width are rounded to multiples of 4; a 1-pixel error at a box edge
    // is invisible and it keeps the bit order of the format out of the code.
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int64_t x1 = static_cast<int64_t>(x) + width;
    int64_t y1 = static_cast<int64_t>(y) + height;
    if (x1 > stride_pixels) x1 = stride_pixels;
    if (y1 > canvas_height) y1 = canvas_height;
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    const uint32_t first_byte = static_cast<uint32_t>(x0) / 4;
    const uint32_t last_byte = static_cast<uint32_t>((x1 + 3) / 4);
    if (last_byte <= first_byte) {
        return;
    }
    const uint32_t byte_count = last_byte - first_byte;
    const uint32_t stride_bytes = stride_pixels / 4;

    // Replicate the 2-bit value into all four pixel slots of a byte.
    const uint8_t pattern = static_cast<uint8_t>((value & 3u) * 0x55u);

    for (int32_t row = y0; row < y1; ++row) {
        memset(canvas + static_cast<size_t>(row) * stride_bytes + first_byte, pattern,
               byte_count);
    }
}

void OverlayRenderer::DrawBox(uint8_t* canvas, uint32_t stride_pixels,
                              uint32_t canvas_height, const MotionRectangle& rectangle,
                              uint8_t value) {
    if (rectangle.width < 2 * kBoxLineWidth || rectangle.height < 2 * kBoxLineWidth) {
        // Too small for a hollow box; a solid blob reads better than nothing.
        FillRect(canvas, stride_pixels, canvas_height, rectangle.x, rectangle.y,
                 rectangle.width, rectangle.height, value);
        return;
    }

    const int32_t x = rectangle.x;
    const int32_t y = rectangle.y;
    const uint32_t w = rectangle.width;
    const uint32_t h = rectangle.height;

    // Top, bottom, left, right.
    FillRect(canvas, stride_pixels, canvas_height, x, y, w, kBoxLineWidth, value);
    FillRect(canvas, stride_pixels, canvas_height, x,
             y + static_cast<int32_t>(h - kBoxLineWidth), w, kBoxLineWidth, value);
    FillRect(canvas, stride_pixels, canvas_height, x, y, kBoxLineWidth, h, value);
    FillRect(canvas, stride_pixels, canvas_height, x + static_cast<int32_t>(w - kBoxLineWidth),
             y, kBoxLineWidth, h, value);
}

}  // namespace baby_monitor
