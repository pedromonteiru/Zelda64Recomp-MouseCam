#include "patches.h"
#include "patch_helpers.h"
#include "z64transition.h"
#include "z64environment.h"
#include "input.h"
#include "z64player.h"
#include "z64light.h"
#include "z64lib.h"

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

// Full screen white fills ("fill white" transitions, mask transformation flash).
RECOMP_PATCH void Environment_FillScreen(GraphicsContext* gfxCtx, u8 red, u8 green, u8 blue, u8 alpha, u8 drawFlags) {
    // Also covers the 220/220/220 flash used when putting on a transformation mask.
    if (recomp_get_dark_transitions_enabled() && red == green && green == blue && red >= 150) {
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

// ---------------------------------------------------------------------------
// @mod Transformation masks: the "put on mask" cutscene floods the screen with a
// bright white/blue fog and light. With Dark Transitions on, those colors are dimmed.
// Values copied from the original table (D_8085D848 in z_player.c).

typedef struct {
    s16 fogNear;
    u8 fogColor[3];
    u8 ambientColor[3];
} ModTransformEnvKey; // matches struct_8085D848_unk_00

typedef struct {
    Vec3f pos;
    u8 color[3];
    s16 radius;
} ModTransformLightKey; // matches struct_8085D848_unk_18

typedef struct {
    ModTransformEnvKey env[3];
    ModTransformLightKey light[3];
} ModTransformFx; // matches struct_8085D848

static const ModTransformFx sModTransformFx[] = {
    {
        {
            { 650, { 0, 0, 0 }, { 10, 0, 30 } },
            { 300, { 200, 200, 255 }, { 0, 0, 0 } },
            { 600, { 0, 0, 0 }, { 0, 0, 200 } },
        },
        {
            { { -40.0f, 20.0f, -10.0f }, { 120, 200, 255 }, 1000 },
            { { 0.0f, -10.0f, 0.0f }, { 255, 255, 255 }, 5000 },
            { { -10.0f, 4.0f, 3.0f }, { 200, 200, 255 }, 5000 },
        },
    },
    {
        {
            { 650, { 0, 0, 0 }, { 10, 0, 30 } },
            { 300, { 200, 200, 255 }, { 0, 0, 0 } },
            { 600, { 0, 0, 0 }, { 0, 0, 200 } },
        },
        {
            { { 0.0f, 0.0f, 5.0f }, { 155, 255, 255 }, 100 },
            { { 0.0f, 0.0f, 5.0f }, { 155, 255, 255 }, 100 },
            { { 0.0f, 0.0f, 5.0f }, { 155, 255, 255 }, 100 },
        },
    },
};

void func_80854EFC(PlayState* play, f32 arg1, ModTransformEnvKey* arg2);
void Player_TranslateAndRotateY(Player* this, Vec3f* translation, Vec3f* src, Vec3f* dst);

static u8 mod_dim(u8 value, s32 percent) {
    return (u8)((value * percent) / 100);
}

RECOMP_PATCH void func_808550D0(PlayState* play, Player* this, f32 arg2, f32 arg3, s32 arg4) {
    ModTransformFx fx;
    ModTransformLightKey* lightInit;
    Vec3f pos;
    s32 i, j;

    // Lib_MemCpy instead of a struct copy (a plain copy would compile to a memcpy call).
    Lib_MemCpy(&fx, (void*)&sModTransformFx[arg4], sizeof(fx));

    if (recomp_get_dark_transitions_enabled()) {
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++) {
                fx.env[i].fogColor[j] = mod_dim(fx.env[i].fogColor[j], 20);
                fx.env[i].ambientColor[j] = mod_dim(fx.env[i].ambientColor[j], 40);
                fx.light[i].color[j] = mod_dim(fx.light[i].color[j], 35);
            }
        }
    }

    func_80854EFC(play, arg2, fx.env);

    lightInit = fx.light;
    if (arg3 > 2.0f) {
        arg3 -= 2.0f;
        lightInit += 2;
    } else if (arg3 > 1.0f) {
        arg3 -= 1.0f;
        lightInit++;
    }

    Player_TranslateAndRotateY(this, &this->actor.world.pos, &lightInit->pos, &pos);
    Lights_PointNoGlowSetInfo(&this->lightInfo, pos.x, pos.y, pos.z, lightInit->color[0], lightInit->color[1],
                              lightInit->color[2], lightInit->radius * arg3);
}
