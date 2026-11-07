/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "supervisor/menu_observer.h"
#include <assert.h>
#include <stdio.h>

static void menu(struct sc_menu_chords *c,struct cs_hotkeys *k,bool down,uint64_t at)
{ cs_hotkeys_menu(k,down,false,at);sc_menu_chords_consume(c,k,true); }

int main(void)
{
    const unsigned int actions[]={BTN_SOUTH,BTN_EAST,BTN_NORTH,BTN_WEST,
        BTN_SELECT,BTN_START,BTN_TL,BTN_TR,BTN_TL2,BTN_TR2,
        BTN_DPAD_LEFT,BTN_DPAD_RIGHT,BTN_DPAD_UP,BTN_DPAD_DOWN,
        KEY_VOLUMEUP,KEY_VOLUMEDOWN,KEY_F24}; /* Unmapped chord also consumes. */
    unsigned int cases=0U;
    for(size_t a=0U;a<sizeof(actions)/sizeof(actions[0]);++a)
        for(unsigned int order=0U;order<2U;++order)
            for(unsigned int release=0U;release<2U;++release)
                for(unsigned int hold=0U;hold<2U;++hold){
                    struct sc_menu_chords c={0};struct cs_hotkeys k;
                    cs_hotkeys_reset(&k,false);
                    if(order==0U){menu(&c,&k,true,100U);sc_menu_chords_event(&c,&k,actions[a],1,KEY_MENU,150U,true);}
                    else {sc_menu_chords_event(&c,&k,actions[a],1,KEY_MENU,90U,true);menu(&c,&k,true,100U);}
                    uint64_t end=hold!=0U?1600U:200U;
                    assert(cs_hotkeys_tick(&k,end)==CS_MENU_NONE);
                    if(release==0U){menu(&c,&k,false,end);sc_menu_chords_event(&c,&k,actions[a],0,KEY_MENU,end+1U,true);}
                    else {sc_menu_chords_event(&c,&k,actions[a],0,KEY_MENU,end,true);menu(&c,&k,false,end+1U);}
                    assert(cs_hotkeys_tick(&k,end+1000U)==CS_MENU_NONE);
                    ++cases;
                }
    {
        struct sc_menu_chords c={0};struct cs_hotkeys k;unsigned char bits[CS_EVDEV_KEY_BYTES]={0};
        bits[BTN_SOUTH/8U]|=(unsigned char)(1U<<(BTN_SOUTH%8U));
        sc_menu_chords_seed(&c,bits,KEY_MENU,50U);cs_hotkeys_reset(&k,false);
        menu(&c,&k,true,100U);assert(cs_hotkeys_tick(&k,900U)==CS_MENU_NONE);
        menu(&c,&k,false,1000U);assert(cs_hotkeys_tick(&k,1500U)==CS_MENU_NONE);
        memset(bits,0,sizeof(bits));sc_menu_chords_seed(&c,bits,KEY_MENU,2000U);
        /* Replayed pre-snapshot press and stray repeat cannot become held. */
        sc_menu_chords_event(&c,&k,BTN_SOUTH,1,KEY_MENU,1999U,true);
        sc_menu_chords_event(&c,&k,BTN_SOUTH,2,KEY_MENU,2001U,true);
        cs_hotkeys_reset(&k,false);menu(&c,&k,true,2100U);menu(&c,&k,false,2150U);
        assert(cs_hotkeys_tick(&k,2400U)==CS_MENU_RECENT);
        cs_hotkeys_reset(&k,false);menu(&c,&k,true,3000U);
        assert(cs_hotkeys_tick(&k,3700U)==CS_MENU_LIBRARY);
        cs_hotkeys_reset(&k,false);menu(&c,&k,true,4000U);menu(&c,&k,false,4050U);
        menu(&c,&k,true,4200U);menu(&c,&k,false,4250U);
        assert(cs_hotkeys_tick(&k,4250U)==CS_MENU_PREVIOUS);
        cs_hotkeys_reset(&k,true);menu(&c,&k,false,5000U);
        assert(cs_hotkeys_tick(&k,5500U)==CS_MENU_NONE);
        cs_hotkeys_reset(&k,false);menu(&c,&k,true,6000U);
        sc_menu_chords_event(&c,&k,KEY_POWER,1,KEY_MENU,6050U,true);
        assert(!k.chord); /* Power retains its separate screenshot/shutdown owner. */
    }
    printf("MENU_PHYSICAL_CHORD_ORDER_RELEASE_HOLD_AND_UNMAPPED_PASS cases=%u\n",cases);
    return 0;
}
