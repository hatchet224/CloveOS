#include "launcher/input.h"
#include <string.h>

static bool profile_valid(const struct cl_input_profile *p)
{
    size_t i, j;
    if (strnlen(p->guid, sizeof(p->guid)) != 32U || p->hat < -1 || p->hat > 255) {
        return false;
    }
    for (i = 0U; i < 32U; ++i) {
        if (!((p->guid[i] >= '0' && p->guid[i] <= '9') ||
              (p->guid[i] >= 'a' && p->guid[i] <= 'f'))) {
            return false;
        }
    }
    for (i = 0U; i < CL_CONTROL_COUNT; ++i) {
        if (p->button[i] < -1 || p->button[i] >= (int)CL_BUTTON_MAX ||
            (i >= CL_A && i <= CL_Y && p->button[i] < 0) ||
            (i < CL_A && p->button[i] < 0 && p->hat < 0)) {
            return false;
        }
        for (j = 0U; j < i; ++j) {
            if (p->button[i] >= 0 && p->button[i] == p->button[j]) {
                return false;
            }
        }
    }
    return true;
}
void cl_input_reference_profile(struct cl_input_profile *p, const char guid[33], bool swap)
{
    static const int buttons[CL_CONTROL_COUNT] = {13, 16, 14, 15, 0, 1, 2, 3, 4, 5, 19, 17, 18, -1, -1};
    memset(p, 0, sizeof(*p));
    if (guid != NULL && strnlen(guid, 33U) == 32U) {
        memcpy(p->guid, guid, 33U);
    }
    memcpy(p->button, buttons, sizeof(buttons));
    p->hat = 0;
    if (swap) {
        p->button[CL_A] = 1;
        p->button[CL_B] = 0;
        p->button[CL_X] = 3;
        p->button[CL_Y] = 2;
    }
}
static uint32_t physical(const struct cl_input *in)
{
    uint32_t keys = in->usb_held;
    unsigned int i;
    for (i = 0U; i < CL_CONTROL_COUNT; ++i) {
        int button = in->profile.button[i];
        if (button >= 0 && in->buttons[(unsigned int)button]) {
            keys |= CL_KEY(i);
        }
    }
    if (in->builtin_l2 && in->buttons[6]) keys |= CL_KEY(CL_L2);
    if (in->builtin_r2 && in->buttons[7]) keys |= CL_KEY(CL_R2);
    if ((in->hat & SDL_HAT_UP) != 0U) {
        keys |= CL_KEY(CL_UP);
    }
    if ((in->hat & SDL_HAT_DOWN) != 0U) {
        keys |= CL_KEY(CL_DOWN);
    }
    if ((in->hat & SDL_HAT_LEFT) != 0U) {
        keys |= CL_KEY(CL_LEFT);
    }
    if ((in->hat & SDL_HAT_RIGHT) != 0U) {
        keys |= CL_KEY(CL_RIGHT);
    }
    if (in->axis_direction[0] < 0) keys |= CL_KEY(CL_LEFT);
    if (in->axis_direction[0] > 0) keys |= CL_KEY(CL_RIGHT);
    if (in->axis_direction[1] < 0) keys |= CL_KEY(CL_UP);
    if (in->axis_direction[1] > 0) keys |= CL_KEY(CL_DOWN);
    return keys;
}
/* Enter beyond half travel; release within one quarter to avoid menu jitter. */
static void navigation_axis(struct cl_input *in, unsigned axis, Sint16 value)
{
    int previous = in->axis_direction[axis];
    in->navigation_axis[axis] = value;
    in->axis_direction[axis] = value <= -16384 ? -1 : value >= 16384 ? 1 :
        previous < 0 && value < -8192 ? -1 : previous > 0 && value > 8192 ? 1 : 0;
}
static uint32_t directions(uint32_t keys)
{
    const uint32_t vertical = CL_KEY(CL_UP) | CL_KEY(CL_DOWN);
    const uint32_t horizontal = CL_KEY(CL_LEFT) | CL_KEY(CL_RIGHT);
    if ((keys & vertical) == vertical) {
        keys &= ~vertical;
    }
    if ((keys & horizontal) == horizontal) {
        keys &= ~horizontal;
    }
    return keys;
}
static bool all_released(const struct cl_input *in)
{
    size_t i;
    if(in->usb_held!=0U)return false;
    if (in->hat != SDL_HAT_CENTERED) {
        return false;
    }
    if (in->axis_direction[0] != 0 || in->axis_direction[1] != 0) return false;
    for (i = 0U; i < CL_BUTTON_MAX; ++i) {
        if (in->buttons[i]) {
            return false;
        }
    }
    return true;
}
static void update(struct cl_input *in, uint64_t now)
{
    uint32_t raw = physical(in), keys = directions(raw);
    int direction;
    if (!in->focused || !in->armed) {
        in->held = 0U;
        in->pressed = 0U;
        in->repeat_direction = 0;
        if (in->focused && raw == 0U && all_released(in)) {
            in->armed = true;
        }
        return;
    }
    in->pressed |= keys & ~in->held;
    in->held = keys;
    direction = (keys & CL_KEY(CL_UP)) != 0U ? -1 : (keys & CL_KEY(CL_DOWN)) != 0U ? 1 : 0;
    if (direction != in->repeat_direction) {
        in->repeat_direction = direction;
        in->repeat_at = now + 300U;
    }
}
void cl_input_rearm(struct cl_input *in)
{
    /* Do not clear physical state: an already held A must be released first. */
    in->armed = false;
    in->held = 0U;
    in->pressed = 0U;
    in->repeat_direction = 0;
}
bool cl_input_neutral(const struct cl_input *in)
{
    return in->joystick != NULL && in->focused && all_released(in);
}
static void sample(struct cl_input *in)
{
    int i, count = SDL_JoystickNumButtons(in->joystick);
    memset(in->buttons, 0, sizeof(in->buttons));
    for (i = 0; i < count && i < (int)CL_BUTTON_MAX; ++i) {
        in->buttons[(unsigned int)i] = SDL_JoystickGetButton(in->joystick, i) != 0U;
    }
    in->hat =
        in->profile.hat >= 0 ? SDL_JoystickGetHat(in->joystick, in->profile.hat) : SDL_HAT_CENTERED;
    for (i = 0; i < 2; ++i)
        navigation_axis(in, (unsigned)i,
            i < in->axis_count ? SDL_JoystickGetAxis(in->joystick, i) : 0);
}
static bool attach(struct cl_input *in, int index)
{
    SDL_Joystick *joystick;
    char guid[33];
    int buttons, hats;
    size_t i;
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, (int)sizeof(guid));
    if (strcmp(guid, in->profile.guid) != 0) {
        return false;
    }
    joystick = SDL_JoystickOpen(index);
    if (joystick == NULL) {
        return false;
    }
    buttons = SDL_JoystickNumButtons(joystick);
    hats = SDL_JoystickNumHats(joystick);
    if (buttons < 0 || buttons > (int)CL_BUTTON_MAX || hats < 0 ||
        (in->profile.hat >= 0 && in->profile.hat >= hats)) {
        SDL_JoystickClose(joystick);
        return false;
    }
    for (i = 0U; i < CL_CONTROL_COUNT; ++i) {
        if (in->profile.button[i] >= buttons) {
            SDL_JoystickClose(joystick);
            return false;
        }
    }
    in->joystick = joystick;
    in->axis_count = SDL_JoystickNumAxes(joystick);
    in->instance = SDL_JoystickInstanceID(joystick);
    SDL_JoystickUpdate();
    sample(in);
    cl_input_rearm(in);
    return true;
}
static void sample_usb(struct cl_input *in)
{
    static const SDL_GameControllerButton map[CL_CONTROL_COUNT]={SDL_CONTROLLER_BUTTON_DPAD_UP,SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT,SDL_CONTROLLER_BUTTON_DPAD_RIGHT,SDL_CONTROLLER_BUTTON_A,SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X,SDL_CONTROLLER_BUTTON_Y,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_GUIDE,SDL_CONTROLLER_BUTTON_INVALID,SDL_CONTROLLER_BUTTON_INVALID,SDL_CONTROLLER_BUTTON_START,SDL_CONTROLLER_BUTTON_BACK};
    in->usb_held=0U;if(in->usb==NULL)return;
    for(unsigned int i=0U;i<CL_CONTROL_COUNT;++i)if(map[i]!=SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(in->usb,map[i])!=0U)in->usb_held|=CL_KEY(i);
    Sint16 x=SDL_GameControllerGetAxis(in->usb,SDL_CONTROLLER_AXIS_LEFTX),y=SDL_GameControllerGetAxis(in->usb,SDL_CONTROLLER_AXIS_LEFTY);
    if(x<=-16384)in->usb_held|=CL_KEY(CL_LEFT);else if(x>=16384)in->usb_held|=CL_KEY(CL_RIGHT);
    if(y<=-16384)in->usb_held|=CL_KEY(CL_UP);else if(y>=16384)in->usb_held|=CL_KEY(CL_DOWN);
    if(SDL_GameControllerGetAxis(in->usb,SDL_CONTROLLER_AXIS_TRIGGERLEFT)>=16384)in->usb_held|=CL_KEY(CL_L2);
    if(SDL_GameControllerGetAxis(in->usb,SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>=16384)in->usb_held|=CL_KEY(CL_R2);
}
static bool attach_usb(struct cl_input *in,int index)
{
    char guid[33];if(in->usb!=NULL || !SDL_IsGameController(index))return false;
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index),guid,sizeof(guid));if(strcmp(guid,in->profile.guid)==0)return false;
    in->usb=SDL_GameControllerOpen(index);if(in->usb==NULL)return false;
    in->usb_instance=SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(in->usb));sample_usb(in);cl_input_rearm(in);return true;
}
bool cl_input_open(struct cl_input *in, const struct cl_input_profile *profile)
{
    int i;
    memset(in, 0, sizeof(*in));
    in->instance = -1;
    in->usb_instance = -1;
    if (profile == NULL || !profile_valid(profile) ||
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
        return false;
    }
    in->profile = *profile;
    /* Zero 28's frozen RetroArch profile assigns raw buttons 6 and 7 to
     * the triggers. Preserve explicit assignments from the controller wizard. */
    if (profile->button[CL_L1] == 4 && profile->button[CL_R1] == 5 &&
        profile->button[CL_MENU] == 19) {
        in->builtin_l2 = true;
        in->builtin_r2 = true;
        for (unsigned int control = 0U; control < CL_CONTROL_COUNT; ++control) {
            in->builtin_l2 &= profile->button[control] != 6;
            in->builtin_r2 &= profile->button[control] != 7;
        }
    }
    in->focused = true;
    (void)SDL_JoystickEventState(SDL_ENABLE);
    for (i = 0; i < SDL_NumJoysticks(); ++i) {
        if (attach(in, i)) {
            break;
        }
    }
    (void)SDL_GameControllerEventState(SDL_ENABLE);
    for(i=0;i<SDL_NumJoysticks();++i)if(attach_usb(in,i))break;
    return true; /* Missing controller is a controller-readable waiting screen. */
}
void cl_input_close(struct cl_input *in)
{
    SDL_GameControllerClose(in->usb);
    SDL_JoystickClose(in->joystick);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS);
    memset(in, 0, sizeof(*in));
    in->instance = -1;
}
bool cl_input_event(struct cl_input *in, const SDL_Event *event, uint64_t now)
{
    bool changed = false;
    if(event->type==SDL_CONTROLLERDEVICEADDED)changed=attach_usb(in,event->cdevice.which);
    else if(event->type==SDL_CONTROLLERDEVICEREMOVED && event->cdevice.which==in->usb_instance){
        SDL_GameControllerClose(in->usb);in->usb=NULL;in->usb_held=0U;in->usb_instance=-1;cl_input_rearm(in);changed=true;
    }else if(in->usb!=NULL && ((event->type==SDL_CONTROLLERBUTTONDOWN || event->type==SDL_CONTROLLERBUTTONUP) && event->cbutton.which==in->usb_instance)){
        sample_usb(in);changed=true;
    }else if(in->usb!=NULL && event->type==SDL_CONTROLLERAXISMOTION && event->caxis.which==in->usb_instance){sample_usb(in);changed=true;}
    else if (event->type == SDL_JOYDEVICEADDED && in->joystick == NULL) {
        changed = attach(in, event->jdevice.which);
    } else if (event->type == SDL_JOYDEVICEREMOVED && event->jdevice.which == in->instance) {
        SDL_JoystickClose(in->joystick);
        in->joystick = NULL;
        in->instance = -1;
        memset(in->buttons, 0, sizeof(in->buttons));
        in->hat = SDL_HAT_CENTERED;
        memset(in->navigation_axis, 0, sizeof(in->navigation_axis));
        memset(in->axis_direction, 0, sizeof(in->axis_direction));
        in->axis_count = 0;
        cl_input_rearm(in);
        changed = true;
    } else if (in->joystick != NULL &&
               (event->type == SDL_JOYBUTTONDOWN || event->type == SDL_JOYBUTTONUP) &&
               event->jbutton.which == in->instance) {
        in->buttons[event->jbutton.button] = event->type == SDL_JOYBUTTONDOWN;
        changed = true;
    } else if (in->joystick != NULL && event->type == SDL_JOYHATMOTION &&
               event->jhat.which == in->instance && (int)event->jhat.hat == in->profile.hat) {
        in->hat = event->jhat.value;
        changed = true;
    } else if (in->joystick != NULL && event->type == SDL_JOYAXISMOTION &&
               event->jaxis.which == in->instance && event->jaxis.axis < 2U &&
               (int)event->jaxis.axis < in->axis_count) {
        navigation_axis(in, event->jaxis.axis, event->jaxis.value);
        changed = true;
    } else if (event->type == SDL_WINDOWEVENT) {
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            in->focused = false;
            cl_input_rearm(in);
            changed = true;
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            in->focused = true;
            if (in->joystick != NULL) {
                sample(in);
            }
            sample_usb(in);
            cl_input_rearm(in);
            changed = true;
        }
    }
    update(in, now);
    /* Keyboard, mouse, touch and controller API duplicates have no mapping. */
    return changed;
}
uint32_t cl_input_take(struct cl_input *in, uint64_t now)
{
    uint32_t keys;
    update(in, now);
    keys = in->pressed;
    in->pressed = 0U;
    if (in->armed && in->repeat_direction != 0 && now >= in->repeat_at) {
        keys |= CL_KEY(in->repeat_direction < 0 ? CL_UP : CL_DOWN);
        in->repeat_at = now + 100U; /* No replay storm after a delayed event pump. */
    }
    return keys;
}
int cl_input_timeout(const struct cl_input *in, uint64_t now)
{
    if (!in->armed || in->repeat_direction == 0) {
        return 16;
    }
    if (now >= in->repeat_at) {
        return 0;
    }
    return in->repeat_at - now > 16U ? 16 : (int)(in->repeat_at - now);
}
