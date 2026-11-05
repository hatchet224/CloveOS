#ifndef LAUNCHER_INPUT_H
#define LAUNCHER_INPUT_H
#include "launcher/model.h"
#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>
#define CL_KEY(control) (UINT32_C(1) << (control))
#define CL_BUTTON_MAX 256U
/* Frontend-only triggers. Leave the persisted controller profile layout intact. */
enum cl_extra_control { CL_L2 = CL_CONTROL_COUNT, CL_R2 };
struct cl_input {
    struct cl_input_profile profile;
    SDL_Joystick *joystick;
    SDL_GameController *usb;
    SDL_JoystickID usb_instance;
    uint32_t usb_held;
    SDL_JoystickID instance;
    bool buttons[CL_BUTTON_MAX];
    Uint8 hat;
    Sint16 navigation_axis[2];
    int axis_direction[2];
    int axis_count;
    uint32_t held, pressed;
    uint64_t repeat_at;
    int repeat_direction;
    bool armed, focused, builtin_l2, builtin_r2;
};
bool cl_input_open(struct cl_input *input, const struct cl_input_profile *profile);
void cl_input_close(struct cl_input *input);
/* Reconnection matches GUID; events match runtime instance ID, never device index. */
bool cl_input_event(struct cl_input *input, const SDL_Event *event, uint64_t now_ms);
uint32_t cl_input_take(struct cl_input *input, uint64_t now_ms);
int cl_input_timeout(const struct cl_input *input, uint64_t now_ms);
void cl_input_rearm(struct cl_input *input);
bool cl_input_neutral(const struct cl_input *input);
/* Reference indices are only a starting profile; actual revision must pass the wizard. */
void cl_input_reference_profile(struct cl_input_profile *profile, const char guid[33],
                                bool swap_face_labels);
#endif
