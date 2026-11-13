#include "patches.h"
#include "patch_helpers.h"
#include "input.h"
#include "sys_matrix.h"

// @mod HUD Scale + HUD layout editor.
// The HUD is drawn in groups (life meter, rupees, magic, minimap, buttons, clock, D-Pad).
// After a group is drawn, its display list commands are rewritten so the group is scaled around
// the screen corner it is attached to and moved by the offset chosen in the HUD editor.
// Texture rectangles, fill rectangles, scissors, RT64 extended texture rectangles and viewports
// (used by the 3D-drawn parts like the A button and the clock) are handled.

#define HUD_OP_TEXRECT       0xE4
#define HUD_OP_TEXRECTFLIP   0xE5
#define HUD_OP_RDPHALF_2     0xF1
#define HUD_OP_FILLRECT      0xF6
#define HUD_OP_SETSCISSOR    0xED
#define HUD_OP_MOVEMEM       0xDC
#define HUD_MV_VIEWPORT      8
#define HUD_OP_RT64_EXTENDED 0x64
#define HUD_EX_TEXRECT_V1    0x000002
#define HUD_EX_ORIGIN_NONE   0x800
#define HUD_OP_MTX           0xDA
#define HUD_MTX_PROJECTION   0x04
#define HUD_MTX_POOL_SIZE    48

// Must match the order of hud_offsets in the host (7 groups).
typedef enum {
    HUD_GROUP_LIFE,
    HUD_GROUP_RUPEES,
    HUD_GROUP_MAGIC,
    HUD_GROUP_MINIMAP,
    HUD_GROUP_BUTTONS,
    HUD_GROUP_CLOCK,
    HUD_GROUP_DPAD,
    HUD_GROUP_COUNT
} HudGroup;

typedef enum {
    HUD_ALIGN_LEFT,
    HUD_ALIGN_RIGHT,
    HUD_ALIGN_NONE,
} HudAlign;

typedef struct {
    HudAlign align;
    s32 margin_x; // Shift applied by the recomp rect alignment (pixels).
    s32 margin_y;
    s16 default_box[4]; // Fallback box (ulx, uly, lrx, lry) when nothing was captured.
} HudGroupInfo;

static const HudGroupInfo sHudGroupInfo[HUD_GROUP_COUNT] = {
    /* LIFE    */ { HUD_ALIGN_LEFT,  -8, -8, { 16, 10, 140, 48 } },
    /* RUPEES  */ { HUD_ALIGN_LEFT,  -8,  8, { 14, 186, 80, 226 } },
    /* MAGIC   */ { HUD_ALIGN_LEFT,  -8, -8, { 14, 30, 130, 46 } },
    /* MINIMAP */ { HUD_ALIGN_RIGHT,  8,  8, { 220, 160, 310, 230 } },
    /* BUTTONS */ { HUD_ALIGN_RIGHT,  8, -8, { 130, 6, 300, 60 } },
    /* CLOCK   */ { HUD_ALIGN_NONE,   0,  8, { 100, 186, 220, 236 } },
    /* DPAD    */ { HUD_ALIGN_LEFT,   0,  0, { 10, 50, 50, 90 } },
};

// Bounding boxes of what each group drew this frame, in the group's own (aligned) space, in pixels.
static f32 sHudBox[HUD_GROUP_COUNT][4];
static s32 sHudBoxValid[HUD_GROUP_COUNT];
// Offsets used this frame (pixels).
static s32 sHudOffX[HUD_GROUP_COUNT];
static s32 sHudOffY[HUD_GROUP_COUNT];

static Gfx* hud_scale_segment_start = NULL;

// Matrices used to move centered (non-aligned) groups like the clock. RT64 handles viewport
// offsets differently for centered viewports, so their 3D parts are moved with the model matrix.
static Mtx sHudMtxPool[2][HUD_MTX_POOL_SIZE];
static s32 sHudMtxBuffer = 0;
static s32 sHudMtxCount = 0;

f32 hud_scale_get(void) {
    s32 percent = recomp_get_hud_scale();
    if (percent <= 0) {
        percent = 100;
    }
    return percent / 100.0f;
}

void hud_box_add(s32 group, f32 ulx, f32 uly, f32 lrx, f32 lry) {
    if (!sHudBoxValid[group]) {
        sHudBox[group][0] = ulx;
        sHudBox[group][1] = uly;
        sHudBox[group][2] = lrx;
        sHudBox[group][3] = lry;
        sHudBoxValid[group] = true;
        return;
    }
    if (ulx < sHudBox[group][0]) sHudBox[group][0] = ulx;
    if (uly < sHudBox[group][1]) sHudBox[group][1] = uly;
    if (lrx > sHudBox[group][2]) sHudBox[group][2] = lrx;
    if (lry > sHudBox[group][3]) sHudBox[group][3] = lry;
}

// Note: helper functions are non-static on purpose (static functions at the end of .text
// confuse the recompiler's static function detection).

// Called once at the start of Interface_Draw.
void hud_scale_frame_start(void) {
    s32 i;
    sHudMtxBuffer ^= 1;
    sHudMtxCount = 0;
    for (i = 0; i < HUD_GROUP_COUNT; i++) {
        sHudBoxValid[i] = false;
        recomp_get_hud_offset(i, &sHudOffX[i], &sHudOffY[i]);
    }
}

void hud_scale_begin(Gfx* gfx) {
    hud_scale_segment_start = gfx;
}

// Scales/moves a coordinate in quarter pixels around an anchor given in pixels.
s32 hud_xform_q(s32 value_q, s32 anchor_px, f32 scale, s32 offset_px) {
    f32 anchor_q = anchor_px * 4.0f;
    s32 result = (s32)(anchor_q + (value_q - anchor_q) * scale + offset_px * 4.0f);
    if (result < 0) {
        result = 0;
    }
    if (result > 0xFFF) {
        result = 0xFFF;
    }
    return result;
}

void hud_scale_end(Gfx* end, s32 group, s32 anchor_x, s32 anchor_y) {
    Gfx* cmd = hud_scale_segment_start;
    f32 scale = hud_scale_get();
    s32 off_x = sHudOffX[group];
    s32 off_y = sHudOffY[group];
    s32 dpad_off_x = sHudOffX[HUD_GROUP_DPAD];
    s32 dpad_off_y = sHudOffY[HUD_GROUP_DPAD];
    bool transform = (scale != 1.0f) || off_x != 0 || off_y != 0 || dpad_off_x != 0 || dpad_off_y != 0;
    // Centered groups (the clock) move their 3D parts through the model matrix instead of the viewport.
    bool matrix_offset = (sHudGroupInfo[group].align == HUD_ALIGN_NONE) && (off_x != 0 || off_y != 0);

    hud_scale_segment_start = NULL;

    if (cmd == NULL) {
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

            if (transform) {
                ulx = hud_xform_q(ulx, anchor_x, scale, off_x);
                lrx = hud_xform_q(lrx, anchor_x, scale, off_x);
                uly = hud_xform_q(uly, anchor_y, scale, off_y);
                lry = hud_xform_q(lry, anchor_y, scale, off_y);

                cmd->words.w0 = (op << 24) | (lrx << 12) | lry;
                cmd->words.w1 = tile | (ulx << 12) | uly;

                // The texture step (dsdx/dtdy) lives in the RDPHALF_2 command that follows.
                if (scale != 1.0f && (cmd + 2) < end && ((cmd[2].words.w0 >> 24) & 0xFF) == HUD_OP_RDPHALF_2) {
                    s16 dsdx = (s16)(cmd[2].words.w1 >> 16);
                    s16 dtdy = (s16)(cmd[2].words.w1 & 0xFFFF);
                    dsdx = (s16)(dsdx / scale);
                    dtdy = (s16)(dtdy / scale);
                    cmd[2].words.w1 = ((u32)(u16)dsdx << 16) | (u16)dtdy;
                }
            }
            hud_box_add(group, ulx / 4.0f, uly / 4.0f, lrx / 4.0f, lry / 4.0f);
        } else if (op == HUD_OP_FILLRECT) {
            s32 lrx = (w0 >> 12) & 0xFFF;
            s32 lry = w0 & 0xFFF;
            s32 ulx = (w1 >> 12) & 0xFFF;
            s32 uly = w1 & 0xFFF;

            if ((lrx - ulx) >= (300 * 4)) {
                continue;
            }

            if (transform) {
                ulx = hud_xform_q(ulx, anchor_x, scale, off_x);
                lrx = hud_xform_q(lrx, anchor_x, scale, off_x);
                uly = hud_xform_q(uly, anchor_y, scale, off_y);
                lry = hud_xform_q(lry, anchor_y, scale, off_y);

                cmd->words.w0 = (op << 24) | (lrx << 12) | lry;
                cmd->words.w1 = (ulx << 12) | uly;
            }
            hud_box_add(group, ulx / 4.0f, uly / 4.0f, lrx / 4.0f, lry / 4.0f);
        } else if (op == HUD_OP_SETSCISSOR) {
            s32 ulx = (w0 >> 12) & 0xFFF;
            s32 uly = w0 & 0xFFF;
            s32 lrx = (w1 >> 12) & 0xFFF;
            s32 lry = w1 & 0xFFF;
            u32 mode = w1 & 0x0F000000;

            if ((lrx - ulx) >= (300 * 4) || !transform) {
                continue;
            }

            ulx = hud_xform_q(ulx, anchor_x, scale, off_x);
            lrx = hud_xform_q(lrx, anchor_x, scale, off_x);
            uly = hud_xform_q(uly, anchor_y, scale, off_y);
            lry = hud_xform_q(lry, anchor_y, scale, off_y);

            cmd->words.w0 = (op << 24) | (ulx << 12) | uly;
            cmd->words.w1 = mode | (lrx << 12) | lry;
        } else if (op == HUD_OP_RT64_EXTENDED && (w0 & 0xFFFFFF) == HUD_EX_TEXRECT_V1 && (cmd + 2) < end) {
            // RT64 extended texture rectangle. With an explicit origin (left/center/right) it belongs to
            // the D-Pad (drawn by the built-in D-Pad mod); otherwise it's part of the current group.
            u32 lorigin = (w1 >> 3) & 0xFFF;
            bool is_dpad = (lorigin != HUD_EX_ORIGIN_NONE);
            s32 target = is_dpad ? HUD_GROUP_DPAD : group;
            f32 ax = is_dpad ? 0.0f : anchor_x * 4.0f;
            f32 ay = anchor_y * 4.0f;
            s32 ox = is_dpad ? dpad_off_x : off_x;
            s32 oy = is_dpad ? dpad_off_y : off_y;
            s16 ulx = (s16)(cmd[1].words.w0 >> 16);
            s16 uly = (s16)(cmd[1].words.w0 & 0xFFFF);
            s16 lrx = (s16)(cmd[1].words.w1 >> 16);
            s16 lry = (s16)(cmd[1].words.w1 & 0xFFFF);

            if (transform) {
                s16 dsdx = (s16)(cmd[2].words.w1 >> 16);
                s16 dtdy = (s16)(cmd[2].words.w1 & 0xFFFF);

                ulx = (s16)(ax + (ulx - ax) * scale + ox * 4);
                lrx = (s16)(ax + (lrx - ax) * scale + ox * 4);
                uly = (s16)(ay + (uly - ay) * scale + oy * 4);
                lry = (s16)(ay + (lry - ay) * scale + oy * 4);
                dsdx = (s16)(dsdx / scale);
                dtdy = (s16)(dtdy / scale);

                cmd[1].words.w0 = ((u32)(u16)ulx << 16) | (u16)uly;
                cmd[1].words.w1 = ((u32)(u16)lrx << 16) | (u16)lry;
                cmd[2].words.w1 = ((u32)(u16)dsdx << 16) | (u16)dtdy;
            }
            hud_box_add(target, ulx / 4.0f, uly / 4.0f, lrx / 4.0f, lry / 4.0f);
            cmd += 2;
        } else if (op == HUD_OP_MTX && matrix_offset && (w0 & HUD_MTX_PROJECTION) == 0) {
            // Model matrix load: copy it with the group's offset added (in ortho units, before the
            // viewport scale, so divide by the scale; ortho Y points up).
            if (w1 >= 0x80000000 && w1 < 0xA0000000 && sHudMtxCount < HUD_MTX_POOL_SIZE) {
                MtxF mf;
                Mtx* dst = &sHudMtxPool[sHudMtxBuffer][sHudMtxCount++];
                Matrix_MtxToMtxF((Mtx*)w1, &mf);
                mf.xw += off_x / scale;
                mf.yw -= off_y / scale;
                Matrix_MtxFToMtx(&mf, dst);
                cmd->words.w1 = (u32)dst;
            }
        } else if (op == HUD_OP_MOVEMEM && (w0 & 0xFF) == HUD_MV_VIEWPORT) {
            // Viewports are freshly allocated every frame, so they can be edited in place.
            if (w1 >= 0x80000000 && w1 < 0xA0000000) {
                Vp* vp = (Vp*)w1;
                f32 ax = anchor_x * 4.0f;
                f32 ay = anchor_y * 4.0f;

                if (transform) {
                    vp->vp.vscale[0] = (s16)(vp->vp.vscale[0] * scale);
                    vp->vp.vscale[1] = (s16)(vp->vp.vscale[1] * scale);
                    s32 vp_off_x = matrix_offset ? 0 : off_x;
                    s32 vp_off_y = matrix_offset ? 0 : off_y;
                    vp->vp.vtrans[0] = (s16)(ax + (vp->vp.vtrans[0] - ax) * scale + vp_off_x * 4);
                    vp->vp.vtrans[1] = (s16)(ay + (vp->vp.vtrans[1] - ay) * scale + vp_off_y * 4);
                }

                // (Viewports are not added to the editor box: they are bigger than what they draw.)
            }
        }
    }

}

// ---------------------------------------------------------------------------------------------
// HUD layout editor (drag and drop).

static s32 sDragGroup = -1;
static f32 sDragStartMouseX;
static f32 sDragStartMouseY;
static s32 sDragStartOffX;
static s32 sDragStartOffY;
static s32 sPrevMouseDown = false;

// Converts the x shift between a group's aligned space and the centered 320x240 space.
f32 hud_group_shift_x(s32 group, f32 left_edge, f32 right_edge) {
    const HudGroupInfo* info = &sHudGroupInfo[group];
    // (if/else instead of switch: jump tables aren't supported in these helper functions)
    if (info->align == HUD_ALIGN_LEFT) {
        return left_edge + info->margin_x;
    }
    if (info->align == HUD_ALIGN_RIGHT) {
        return right_edge - SCREEN_WIDTH + info->margin_x;
    }
    return info->margin_x;
}

// Group box in the group's aligned space (pixels), using the fallback box if nothing was captured.
bool hud_group_box(s32 group, f32* box) {
    if (sHudBoxValid[group]) {
        box[0] = sHudBox[group][0];
        box[1] = sHudBox[group][1];
        box[2] = sHudBox[group][2];
        box[3] = sHudBox[group][3];
        return true;
    }
    return false;
}

void hud_set_align(Gfx** gfxp, s32 group) {
    const HudGroupInfo* info = &sHudGroupInfo[group];
    Gfx* gfx = *gfxp;
    if (info->align == HUD_ALIGN_LEFT) {
        gEXSetRectAlign(gfx++, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_LEFT, info->margin_x * 4, info->margin_y * 4,
                        info->margin_x * 4, info->margin_y * 4);
    } else if (info->align == HUD_ALIGN_RIGHT) {
        gEXSetRectAlign(gfx++, G_EX_ORIGIN_RIGHT, G_EX_ORIGIN_RIGHT, (-SCREEN_WIDTH + info->margin_x) * 4,
                        info->margin_y * 4, (-SCREEN_WIDTH + info->margin_x) * 4, info->margin_y * 4);
    } else {
        gEXSetRectAlign(gfx++, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, info->margin_x * 4, info->margin_y * 4,
                        info->margin_x * 4, info->margin_y * 4);
    }
    *gfxp = gfx;
}

void hud_fill(Gfx** gfxp, f32 ulx, f32 uly, f32 lrx, f32 lry) {
    Gfx* gfx = *gfxp;
    if (ulx < 0) ulx = 0;
    if (uly < 0) uly = 0;
    if (lrx > 1000) lrx = 1000;
    if (lry > 1000) lry = 1000;
    if (lrx <= ulx || lry <= uly) {
        return;
    }
    gDPFillRectangle(gfx++, (s32)ulx, (s32)uly, (s32)lrx, (s32)lry);
    *gfxp = gfx;
}

// Called at the end of the HUD drawing. Handles dragging and draws the group outlines.
void hud_editor_update_and_draw(Gfx** gfxp) {
    f32 mouse[2];
    f32 edges[2];
    s32 mouse_down;
    s32 group;
    s32 hovered = -1;
    Gfx* gfx;

    if (!recomp_hud_editor_poll(mouse, &mouse_down, edges)) {
        sDragGroup = -1;
        sPrevMouseDown = false;
        return;
    }

    // Find the group under the mouse (D-Pad first since it's small and can sit inside other boxes).
    for (group = HUD_GROUP_COUNT - 1; group >= 0; group--) {
        f32 box[4];
        f32 shift_x, shift_y;
        if (!hud_group_box(group, box)) {
            continue;
        }
        shift_x = hud_group_shift_x(group, edges[0], edges[1]);
        shift_y = sHudGroupInfo[group].margin_y;
        if (mouse[0] >= box[0] + shift_x && mouse[0] <= box[2] + shift_x &&
            mouse[1] >= box[1] + shift_y && mouse[1] <= box[3] + shift_y) {
            hovered = group;
            break;
        }
    }

    if (mouse_down && !sPrevMouseDown && hovered >= 0) {
        sDragGroup = hovered;
        sDragStartMouseX = mouse[0];
        sDragStartMouseY = mouse[1];
        sDragStartOffX = sHudOffX[hovered];
        sDragStartOffY = sHudOffY[hovered];
    }
    if (!mouse_down) {
        sDragGroup = -1;
    }

    if (sDragGroup >= 0) {
        f32 box[4];
        s32 new_x = sDragStartOffX + (s32)(mouse[0] - sDragStartMouseX);
        s32 new_y = sDragStartOffY + (s32)(mouse[1] - sDragStartMouseY);

        // Keep the group on screen.
        if (hud_group_box(sDragGroup, box)) {
            f32 shift_x = hud_group_shift_x(sDragGroup, edges[0], edges[1]);
            f32 shift_y = sHudGroupInfo[sDragGroup].margin_y;
            // Box without the current offset, in centered space.
            f32 base_ulx = box[0] - sHudOffX[sDragGroup] + shift_x;
            f32 base_lrx = box[2] - sHudOffX[sDragGroup] + shift_x;
            f32 base_uly = box[1] - sHudOffY[sDragGroup] + shift_y;
            f32 base_lry = box[3] - sHudOffY[sDragGroup] + shift_y;
            if (base_ulx + new_x < edges[0]) new_x = (s32)(edges[0] - base_ulx);
            if (base_lrx + new_x > edges[1]) new_x = (s32)(edges[1] - base_lrx);
            if (base_uly + new_y < 0) new_y = (s32)(-base_uly);
            if (base_lry + new_y > SCREEN_HEIGHT) new_y = (s32)(SCREEN_HEIGHT - base_lry);
        }

        recomp_set_hud_offset(sDragGroup, new_x, new_y);
    }

    sPrevMouseDown = mouse_down;

    // Draw the outlines.
    gfx = *gfxp;
    gDPPipeSync(gfx++);
    gDPSetCycleType(gfx++, G_CYC_1CYCLE);
    gDPSetRenderMode(gfx++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(gfx++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);

    for (group = 0; group < HUD_GROUP_COUNT; group++) {
        f32 box[4];
        bool active = (group == sDragGroup) || (sDragGroup < 0 && group == hovered);
        if (!hud_group_box(group, box)) {
            continue;
        }

        hud_set_align(&gfx, group);

        // Light fill only on the element under the mouse / being dragged.
        if (active) {
            gDPSetPrimColor(gfx++, 0, 0, 255, 220, 60, 50);
            hud_fill(&gfx, box[0], box[1], box[2], box[3]);
        }

        // Thin border (1 pixel), tight around the element.
        if (active) {
            gDPSetPrimColor(gfx++, 0, 0, 255, 220, 60, 230);
        } else {
            gDPSetPrimColor(gfx++, 0, 0, 255, 255, 255, 140);
        }
        hud_fill(&gfx, box[0], box[1], box[2], box[1] + 1);
        hud_fill(&gfx, box[0], box[3] - 1, box[2], box[3]);
        hud_fill(&gfx, box[0], box[1], box[0] + 1, box[3]);
        hud_fill(&gfx, box[2] - 1, box[1], box[2], box[3]);
    }

    gEXSetRectAlign(gfx++, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, 0, 0, 0, 0);
    gDPPipeSync(gfx++);
    *gfxp = gfx;
}
