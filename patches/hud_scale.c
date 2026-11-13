#include "patches.h"
#include "patch_helpers.h"
#include "input.h"

// @mod HUD Scale.
// The HUD is drawn in groups (life meter, rupees, magic, minimap, buttons, clock, minigame icons).
// After a group is drawn, its display list commands are rewritten so the group is scaled around
// the screen corner it is attached to (e.g. hearts around the top-left corner, buttons around the
// top-right corner). Texture rectangles, fill rectangles, scissors and the viewport (used by the
// 3D-drawn parts like the A button and the clock) are scaled; everything else is left untouched.

#define HUD_OP_TEXRECT      0xE4
#define HUD_OP_TEXRECTFLIP  0xE5
#define HUD_OP_RDPHALF_1    0xE1
#define HUD_OP_RDPHALF_2    0xF1
#define HUD_OP_FILLRECT     0xF6
#define HUD_OP_SETSCISSOR   0xED
#define HUD_OP_MOVEMEM      0xDC
#define HUD_MV_VIEWPORT     8
#define HUD_OP_RT64_EXTENDED 0x64
#define HUD_EX_TEXRECT_V1   0x000002
#define HUD_EX_ORIGIN_NONE  0x800

static Gfx* hud_scale_segment_start = NULL;

f32 hud_scale_get(void) {
    s32 percent = recomp_get_hud_scale();
    if (percent <= 0) {
        percent = 100;
    }
    return percent / 100.0f;
}

void hud_scale_begin(Gfx* gfx) {
    hud_scale_segment_start = gfx;
}

// Scales a coordinate in 10.2 fixed point (quarter pixels) around an anchor given in pixels.
static s32 hud_scale_coord(s32 value_q, s32 anchor_px, f32 scale) {
    f32 anchor_q = anchor_px * 4.0f;
    s32 result = (s32)(anchor_q + (value_q - anchor_q) * scale);
    if (result < 0) {
        result = 0;
    }
    if (result > 0xFFF) {
        result = 0xFFF;
    }
    return result;
}

void hud_scale_end(Gfx* end, s32 anchor_x, s32 anchor_y) {
    Gfx* cmd = hud_scale_segment_start;
    f32 scale = hud_scale_get();

    hud_scale_segment_start = NULL;

    if (cmd == NULL || scale == 1.0f) {
        return;
    }

    for (; cmd < end; cmd++) {
        u32 w0 = cmd->words.w0;
        u32 w1 = cmd->words.w1;
        u32 op = (w0 >> 24) & 0xFF;

        if (op == HUD_OP_TEXRECT || op == HUD_OP_TEXRECTFLIP) {
            s32 lrx = (w0 >> 12) & 0xFFF;
            s32 lry = w0 & 0xFFF;
            s32 ulx = (w1 >> 12) & 0xFFF;
            s32 uly = w1 & 0xFFF;
            u32 tile = w1 & 0x0F000000;

            // Leave full-screen rectangles alone.
            if ((lrx - ulx) >= (300 * 4)) {
                continue;
            }

            ulx = hud_scale_coord(ulx, anchor_x, scale);
            lrx = hud_scale_coord(lrx, anchor_x, scale);
            uly = hud_scale_coord(uly, anchor_y, scale);
            lry = hud_scale_coord(lry, anchor_y, scale);

            cmd->words.w0 = (op << 24) | (lrx << 12) | lry;
            cmd->words.w1 = tile | (ulx << 12) | uly;

            // The texture step (dsdx/dtdy) lives in the RDPHALF_2 command that follows.
            if ((cmd + 2) < end && ((cmd[2].words.w0 >> 24) & 0xFF) == HUD_OP_RDPHALF_2) {
                s16 dsdx = (s16)(cmd[2].words.w1 >> 16);
                s16 dtdy = (s16)(cmd[2].words.w1 & 0xFFFF);
                dsdx = (s16)(dsdx / scale);
                dtdy = (s16)(dtdy / scale);
                cmd[2].words.w1 = ((u32)(u16)dsdx << 16) | (u16)dtdy;
            }
        } else if (op == HUD_OP_FILLRECT) {
            s32 lrx = (w0 >> 12) & 0xFFF;
            s32 lry = w0 & 0xFFF;
            s32 ulx = (w1 >> 12) & 0xFFF;
            s32 uly = w1 & 0xFFF;

            if ((lrx - ulx) >= (300 * 4)) {
                continue;
            }

            ulx = hud_scale_coord(ulx, anchor_x, scale);
            lrx = hud_scale_coord(lrx, anchor_x, scale);
            uly = hud_scale_coord(uly, anchor_y, scale);
            lry = hud_scale_coord(lry, anchor_y, scale);

            cmd->words.w0 = (op << 24) | (lrx << 12) | lry;
            cmd->words.w1 = (ulx << 12) | uly;
        } else if (op == HUD_OP_SETSCISSOR) {
            s32 ulx = (w0 >> 12) & 0xFFF;
            s32 uly = w0 & 0xFFF;
            s32 lrx = (w1 >> 12) & 0xFFF;
            s32 lry = w1 & 0xFFF;
            u32 mode = w1 & 0x0F000000;

            if ((lrx - ulx) >= (300 * 4)) {
                continue;
            }

            ulx = hud_scale_coord(ulx, anchor_x, scale);
            lrx = hud_scale_coord(lrx, anchor_x, scale);
            uly = hud_scale_coord(uly, anchor_y, scale);
            lry = hud_scale_coord(lry, anchor_y, scale);

            cmd->words.w0 = (op << 24) | (ulx << 12) | uly;
            cmd->words.w1 = mode | (lrx << 12) | lry;
        } else if (op == HUD_OP_RT64_EXTENDED && (w0 & 0xFFFFFF) == HUD_EX_TEXRECT_V1 && (cmd + 2) < end) {
            // RT64 extended texture rectangle (used by e.g. the D-Pad mod). Coordinates are signed
            // quarter pixels relative to the chosen origin; with an explicit origin (left/center/right)
            // we scale around that origin, otherwise around the group's anchor.
            u32 lorigin = (w1 >> 3) & 0xFFF;
            f32 ax = (lorigin == HUD_EX_ORIGIN_NONE) ? anchor_x * 4.0f : 0.0f;
            f32 ay = anchor_y * 4.0f;
            s16 ulx = (s16)(cmd[1].words.w0 >> 16);
            s16 uly = (s16)(cmd[1].words.w0 & 0xFFFF);
            s16 lrx = (s16)(cmd[1].words.w1 >> 16);
            s16 lry = (s16)(cmd[1].words.w1 & 0xFFFF);
            s16 dsdx = (s16)(cmd[2].words.w1 >> 16);
            s16 dtdy = (s16)(cmd[2].words.w1 & 0xFFFF);

            ulx = (s16)(ax + (ulx - ax) * scale);
            lrx = (s16)(ax + (lrx - ax) * scale);
            uly = (s16)(ay + (uly - ay) * scale);
            lry = (s16)(ay + (lry - ay) * scale);
            dsdx = (s16)(dsdx / scale);
            dtdy = (s16)(dtdy / scale);

            cmd[1].words.w0 = ((u32)(u16)ulx << 16) | (u16)uly;
            cmd[1].words.w1 = ((u32)(u16)lrx << 16) | (u16)lry;
            cmd[2].words.w1 = ((u32)(u16)dsdx << 16) | (u16)dtdy;
            cmd += 2;
        } else if (op == HUD_OP_MOVEMEM && (w0 & 0xFF) == HUD_MV_VIEWPORT) {
            // Viewports are freshly allocated every frame, so they can be edited in place.
            if (w1 >= 0x80000000 && w1 < 0xA0000000) {
                Vp* vp = (Vp*)w1;
                f32 ax = anchor_x * 4.0f;
                f32 ay = anchor_y * 4.0f;

                vp->vp.vscale[0] = (s16)(vp->vp.vscale[0] * scale);
                vp->vp.vscale[1] = (s16)(vp->vp.vscale[1] * scale);
                vp->vp.vtrans[0] = (s16)(ax + (vp->vp.vtrans[0] - ax) * scale);
                vp->vp.vtrans[1] = (s16)(ay + (vp->vp.vtrans[1] - ay) * scale);
            }
        }
    }
}
