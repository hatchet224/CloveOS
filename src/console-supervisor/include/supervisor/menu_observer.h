#ifndef SUPERVISOR_MENU_OBSERVER_H
#define SUPERVISOR_MENU_OBSERVER_H
#include "sessiond/hotkeys.h"
#include "sessiond/evdev.h"
#include <string.h>

/* Call only after draining the event queue and outside SYN_DROPPED recovery.
 * The key bitmap is a fresh physical observation, not a replayed event. Keep
 * an ordering barrier so the queued copy of this edge cannot trigger twice. */
static inline bool sc_menu_reconcile(struct cs_hotkeys *keys,bool held,
                                     uint64_t now,uint64_t *sampled_at)
{
    if (held==keys->down || now<*sampled_at) return false;
    cs_hotkeys_menu(keys,held,false,now);
    *sampled_at=now;
    return true;
}
/* Track physical companion keys even before MENU is pressed. This consumes
 * a chord, including an unmapped one, without assigning it a new action. */
struct sc_menu_chords {
    unsigned char held[CS_EVDEV_KEY_BYTES];
    uint64_t sampled_at;
};
static inline bool sc_menu_chord_key(unsigned int code,unsigned int menu_code)
{ return code>KEY_RESERVED && code<KEY_CNT && code!=menu_code && code!=KEY_POWER; }
static inline void sc_menu_chords_consume(const struct sc_menu_chords *chords,
                                        struct cs_hotkeys *keys,bool active)
{
    if(!active || !keys->down || !keys->armed)return;
    for(size_t i=0U;i<sizeof(chords->held);++i)
        if(chords->held[i]!=0U){cs_hotkeys_consume(keys);break;}
}
static inline void sc_menu_chords_seed(struct sc_menu_chords *chords,
        const unsigned char bitmap[CS_EVDEV_KEY_BYTES],unsigned int menu_code,uint64_t at)
{
    memset(chords->held,0,sizeof(chords->held));
    for(unsigned int code=1U;code<KEY_CNT;++code)
        if(sc_menu_chord_key(code,menu_code) && (bitmap[code/8U]&(1U<<(code%8U)))!=0U)
            chords->held[code/8U]|=(unsigned char)(1U<<(code%8U));
    chords->sampled_at=at;
}
static inline void sc_menu_chords_event(struct sc_menu_chords *chords,struct cs_hotkeys *keys,
        unsigned int code,int value,unsigned int menu_code,uint64_t at,bool active)
{
    if(!sc_menu_chord_key(code,menu_code) || at<=chords->sampled_at || value<0 || value>2)return;
    unsigned char mask=(unsigned char)(1U<<(code%8U));
    if(value==1)chords->held[code/8U]|=mask;
    else if(value==0)chords->held[code/8U]&=(unsigned char)~mask;
    sc_menu_chords_consume(chords,keys,active);
}
#endif
