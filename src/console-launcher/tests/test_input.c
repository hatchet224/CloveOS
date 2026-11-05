#include "launcher/input.h"
#undef NDEBUG
#include <assert.h>
#include <string.h>
#include <stdio.h>

#if SDL_VERSION_ATLEAST(2, 0, 14)
static void button(struct cl_input *in, enum cl_control control, bool down, uint64_t now)
{
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = down ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
    event.jbutton.which = in->instance;
    event.jbutton.button = (Uint8)in->profile.button[control];
    (void)cl_input_event(in, &event, now);
}
#endif
int main(void)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    struct cl_input input;
    struct cl_input_profile profile;
    char guid[33];
    SDL_Event event;
    int device;
    assert(SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) == 0);
    device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 4, 20, 1);
    assert(device >= 0);
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(device), guid, (int)sizeof(guid));
    cl_input_reference_profile(&profile, guid, false);
    profile.button[CL_START]=6;
    profile.button[CL_SELECT]=7;
    assert(cl_input_open(&input, &profile));
    assert(input.joystick != NULL);
    assert(cl_input_take(&input, 0U) == 0U);
    memset(&event, 0, sizeof(event));
    event.type = SDL_JOYAXISMOTION;
    event.jaxis.which = input.instance;
    event.jaxis.axis = 1U;
    event.jaxis.value = 20000;
    (void)cl_input_event(&input, &event, 0U);
    assert(cl_input_take(&input, 0U) == CL_KEY(CL_DOWN));
    assert(cl_input_take(&input, 299U) == 0U);
    assert(cl_input_take(&input, 300U) == CL_KEY(CL_DOWN));
    event.jaxis.value = 10000;
    (void)cl_input_event(&input, &event, 301U);
    assert(input.held == CL_KEY(CL_DOWN)); /* hysteresis */
    cl_input_rearm(&input);
    assert(!cl_input_neutral(&input));
    assert(cl_input_take(&input, 400U) == 0U);
    event.jaxis.value = 0;
    (void)cl_input_event(&input, &event, 401U);
    assert(cl_input_neutral(&input));
    event.jaxis.which = input.instance + 1;
    event.jaxis.value = -32768;
    (void)cl_input_event(&input, &event, 402U);
    assert(cl_input_take(&input, 402U) == 0U);
    event.jaxis.which = input.instance;
    event.jaxis.axis = 2U; /* right stick cannot accidentally navigate */
    (void)cl_input_event(&input, &event, 403U);
    assert(cl_input_take(&input, 403U) == 0U);
    button(&input, CL_DOWN, true, 0U);
    assert(cl_input_take(&input, 0U) == CL_KEY(CL_DOWN));
    assert(cl_input_take(&input, 299U) == 0U);
    assert(cl_input_take(&input, 300U) == CL_KEY(CL_DOWN));
    assert(cl_input_take(&input, 399U) == 0U);
    assert(cl_input_take(&input, 400U) == CL_KEY(CL_DOWN));
    assert(cl_input_take(&input, 10000U) == CL_KEY(CL_DOWN));
    assert(cl_input_take(&input, 10000U) == 0U); /* no delayed-repeat burst */
    button(&input, CL_DOWN, false, 10001U);
    button(&input, CL_A, true, 10002U);
    assert(cl_input_take(&input, 10002U) == CL_KEY(CL_A));
    assert(cl_input_take(&input, 11000U) == 0U); /* face buttons never repeat */
    cl_input_rearm(&input);
    assert(cl_input_take(&input, 12000U) == 0U); /* held A cannot accept a dialog */
    button(&input, CL_A, false, 12001U);
    button(&input, CL_A, true, 12002U);
    assert(cl_input_take(&input, 12002U) == CL_KEY(CL_A));
    button(&input, CL_A, false, 12003U);
    button(&input, CL_START, true, 12003U);
    assert(cl_input_take(&input, 12003U)==CL_KEY(CL_START));
    button(&input, CL_START, false, 12003U);
    button(&input, CL_SELECT, true, 12003U);
    assert(cl_input_take(&input, 12003U)==CL_KEY(CL_SELECT));
    button(&input, CL_SELECT, false, 12003U);
    memset(&event, 0, sizeof(event));
    event.type = SDL_KEYDOWN;
    event.key.keysym.sym = SDLK_RETURN;
    (void)cl_input_event(&input, &event, 12004U);
    assert(cl_input_take(&input, 12004U) == 0U);
    event.type = SDL_MOUSEBUTTONDOWN;
    (void)cl_input_event(&input, &event, 12004U);
    event.type = SDL_FINGERDOWN;
    (void)cl_input_event(&input, &event, 12004U);
    assert(cl_input_take(&input, 12004U) == 0U);
    event.type = SDL_JOYHATMOTION;
    event.jhat.which = input.instance;
    event.jhat.hat = 0U;
    event.jhat.value = SDL_HAT_UP;
    (void)cl_input_event(&input, &event, 12005U);
    assert(cl_input_take(&input, 12005U) == CL_KEY(CL_UP));
    button(&input, CL_UP, true, 12006U);
    assert(cl_input_take(&input, 12006U) == 0U); /* hat/button duplicate */
    event.jhat.value = SDL_HAT_CENTERED;
    (void)cl_input_event(&input, &event, 12007U);
    assert(input.held == CL_KEY(CL_UP)); /* button still down */
    button(&input, CL_DOWN, true, 12008U);
    assert(input.held == 0U); /* opposing directions cancel */
    memset(&event, 0, sizeof(event));
    event.type = SDL_JOYBUTTONDOWN;
    event.jbutton.which = input.instance + 1;
    event.jbutton.button = 0U;
    (void)cl_input_event(&input, &event, 12009U);
    assert((cl_input_take(&input, 12009U) & CL_KEY(CL_A)) == 0U);
    button(&input,CL_UP,false,12010U);button(&input,CL_DOWN,false,12011U);
#if SDL_VERSION_ATLEAST(2,24,0)
    {
        SDL_VirtualJoystickDesc descriptor;memset(&descriptor,0,sizeof(descriptor));descriptor.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        descriptor.type=SDL_JOYSTICK_TYPE_GAMECONTROLLER;descriptor.naxes=6;descriptor.nbuttons=15;descriptor.nhats=1;
        descriptor.vendor_id=0x1234;descriptor.product_id=0x9876;descriptor.name="USB control test";
        int usb=SDL_JoystickAttachVirtualEx(&descriptor);assert(usb>=0);
        char usb_guid[33],mapping[1024];SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(usb),usb_guid,sizeof(usb_guid));
        snprintf(mapping,sizeof(mapping),"%s,USB control test,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,platform:Linux,",usb_guid);
        assert(SDL_GameControllerAddMapping(mapping)>=0);
        memset(&event,0,sizeof(event));event.type=SDL_CONTROLLERDEVICEADDED;event.cdevice.which=usb;
        assert(cl_input_event(&input,&event,12012U));assert(input.usb!=NULL);
        SDL_Joystick *physical=SDL_JoystickOpen(usb);assert(physical!=NULL);
        (void)cl_input_take(&input,12013U);
        assert(SDL_JoystickSetVirtualButton(physical,0,1)==0);SDL_JoystickUpdate();
        event.type=SDL_CONTROLLERBUTTONDOWN;event.cbutton.which=input.usb_instance;event.cbutton.button=SDL_CONTROLLER_BUTTON_A;
        assert(cl_input_event(&input,&event,12014U));assert(cl_input_take(&input,12014U)==CL_KEY(CL_A));
        assert(SDL_JoystickSetVirtualButton(physical,0,0)==0);SDL_JoystickUpdate();event.type=SDL_CONTROLLERBUTTONUP;
        assert(cl_input_event(&input,&event,12015U));assert(input.held==0U);
        /* SDL's virtual gamecontroller driver supplies its own logical dpad
         * button mapping, independent of the mapping database override. */
        assert(SDL_JoystickSetVirtualButton(physical,13,1)==0);SDL_JoystickUpdate();
        event.type=SDL_CONTROLLERBUTTONDOWN;event.cbutton.button=SDL_CONTROLLER_BUTTON_DPAD_LEFT;
        assert(cl_input_event(&input,&event,12016U));
        uint32_t observed=cl_input_take(&input,12016U);
        if(observed!=CL_KEY(CL_LEFT))fprintf(stderr,"usb left observed=%u usb_held=%u held=%u hat=%u\n",observed,input.usb_held,input.held,SDL_JoystickGetHat(physical,0));
        assert(observed==CL_KEY(CL_LEFT));
        assert(SDL_JoystickSetVirtualButton(physical,13,0)==0);
        assert(SDL_JoystickSetVirtualAxis(physical,4,32767)==0);SDL_JoystickUpdate();
        event.type=SDL_CONTROLLERAXISMOTION;event.caxis.which=input.usb_instance;
        event.caxis.axis=SDL_CONTROLLER_AXIS_TRIGGERLEFT;
        assert(cl_input_event(&input,&event,12017U));
        assert(cl_input_take(&input,12017U)==CL_KEY(CL_L2));
        event.type=SDL_CONTROLLERDEVICEREMOVED;event.cdevice.which=input.usb_instance;
        assert(cl_input_event(&input,&event,12017U));assert(input.usb==NULL && input.usb_held==0U && input.joystick!=NULL);
        SDL_JoystickClose(physical);assert(SDL_JoystickDetachVirtual(usb)==0);
        puts("USB_CONTROLLER_MAPPING_NEUTRAL_UNPLUG_PASS");
    }
#endif
    event.type = SDL_JOYDEVICEREMOVED;
    event.jdevice.which = input.instance;
    (void)cl_input_event(&input, &event, 12010U);
    assert(input.joystick == NULL && input.held == 0U);
    cl_input_close(&input);
    profile.button[CL_START]=9;
    profile.button[CL_SELECT]=8;
    assert(cl_input_open(&input,&profile));
    assert(cl_input_take(&input,0U)==0U);
    memset(&event,0,sizeof(event));
    event.type=SDL_JOYBUTTONDOWN;
    event.jbutton.which=input.instance;
    event.jbutton.button=6U;
    assert(cl_input_event(&input,&event,1U));
    assert(cl_input_take(&input,1U)==CL_KEY(CL_L2));
    event.jbutton.button=7U;
    assert(cl_input_event(&input,&event,2U));
    assert(cl_input_take(&input,2U)==CL_KEY(CL_R2));
    cl_input_close(&input);
    assert(SDL_JoystickDetachVirtual(device) == 0);
    SDL_Quit();
    puts("START_SELECT_VIRTUAL_INPUT_PASS repeat/focus/rearm/no-pointer/physical-button-events");
    puts("BUILTIN_AND_USB_TRIGGER_INPUT_PASS");
    return 0;
#else
    return 77; /* Production input remains compatible with SDL >=2.0.5. */
#endif
}
