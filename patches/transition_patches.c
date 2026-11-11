#include "patches.h"
#include "patch_helpers.h"
#include "z64transition.h"
#include "z64environment.h"
#include "input.h"

// @mod Option to replace the bright white screen transitions with black ones.

#define WHITE_TRANSITION_COLOR RGBA8(160, 160, 160, 255)
#define TRANS_FADE_TYPE_FLASH_VALUE 2

// Fades between areas (day "dynamic" fades, Song of Time, etc).
RECOMP_PATCH void TransitionFade_SetColor(void* thisx, u32 color) {
    TransitionFade* this = (TransitionFade*)thisx;

    // Keep flashes (camera/pictograph style) as they are; only change transitions.
    if (recomp_get_dark_transitions_enabled() && this->type != TRANS_FADE_TYPE_FLASH_VALUE && color == WHITE_TRANSITION_COLOR) {
        color = RGBA8(0, 0, 0, 255);
    }

    this->color.rgba = color;
}

// Full screen fills used by the "fill white" transitions.
RECOMP_PATCH void Environment_FillScreen(GraphicsContext* gfxCtx, u8 red, u8 green, u8 blue, u8 alpha, u8 drawFlags) {
    if (recomp_get_dark_transitions_enabled() && red == 160 && green == 160 && blue == 160) {
        red = 0;
        green = 0;
        blue = 0;
    }

    if (alpha != 0) {
        OPEN_DISPS(gfxCtx);

        if (drawFlags & FILL_SCREEN_OPA) {
            POLY_OPA_DISP = Gfx_SetupDL57(POLY_OPA_DISP);
            gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, red, green, blue, alpha);
            gDPSetAlphaDither(POLY_OPA_DISP++, G_AD_DISABLE);
            gDPSetColorDither(POLY_OPA_DISP++, G_CD_DISABLE);
            gSPDisplayList(POLY_OPA_DISP++, D_0E000000.clearFillRect);
        }

        if (drawFlags & FILL_SCREEN_XLU) {
            POLY_XLU_DISP = Gfx_SetupDL57(POLY_XLU_DISP);
            gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, red, green, blue, alpha);

            if ((u32)alpha == 255) {
                gDPSetRenderMode(POLY_XLU_DISP++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
            }

            gDPSetAlphaDither(POLY_XLU_DISP++, G_AD_DISABLE);
            gDPSetColorDither(POLY_XLU_DISP++, G_CD_DISABLE);
            gSPDisplayList(POLY_XLU_DISP++, D_0E000000.clearFillRect);
        }

        CLOSE_DISPS(gfxCtx);
    }
}
