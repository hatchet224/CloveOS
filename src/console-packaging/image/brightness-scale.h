/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CLOVE_BRIGHTNESS_SCALE_H
#define CLOVE_BRIGHTNESS_SCALE_H
#include <limits.h>
static const unsigned int cl_brightness_steps[]={8U,16U,32U,48U,72U,96U,128U,160U,192U,255U};
/* Logical selected levels are1..10, independent of transient driver readback. */
static inline unsigned int cl_brightness_raw(unsigned int level)
{ return level>=1U && level<=10U?cl_brightness_steps[level-1U]:0U; }
static inline unsigned int cl_brightness_level(unsigned int raw)
{
    unsigned int best=0U,distance=UINT_MAX;
    for(unsigned int i=0U;i<10U;++i){
        unsigned int value=cl_brightness_steps[i],delta=value>raw?value-raw:raw-value;
        if(delta<distance){best=i;distance=delta;}
    }
    return best+1U;
}
/* R52/R53 schema2 stored inverse native PWM, as displayed by their slider.
 * Authenticate the old record before converting. Schema3 never converts again. */
static inline unsigned int cl_brightness_legacy_level(unsigned int raw)
{ return raw==0U?0U:11U-cl_brightness_level(raw); }
#endif
