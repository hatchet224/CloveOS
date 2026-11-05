/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MAGICX_SATURN_POLICY_H
#define MAGICX_SATURN_POLICY_H
#include <stdbool.h>
#include <stdint.h>

struct magicx_saturn_sticks {
    bool armed[2];
    unsigned neutral[2];
    unsigned diagnostics;
};

/* Sample once per physical poll, never once per +/- axis query. Buttons and
 * the physical D-pad do not pass through this gate. A stick held at launch
 * must be released before it can synthesize a direction. */
static inline void magicx_saturn_sticks_poll(struct magicx_saturn_sticks *s,
                                            const int16_t axes[4])
{
    unsigned pair;
    for (pair=0;pair<2;pair++) {
        int x=axes[pair*2],y=axes[pair*2+1];
        if (s->armed[pair]) continue;
        if (x>=-8192 && x<=8192 && y>=-8192 && y<=8192) {
            if (++s->neutral[pair]>=2) s->armed[pair]=true;
        } else s->neutral[pair]=0;
    }
}

static inline int16_t magicx_saturn_stick_axis(
        const struct magicx_saturn_sticks *s,unsigned axis,int16_t value)
{
    if (axis>=4) return value;
    if (!s->armed[axis/2]) return 0;
    /* Saturn's digital pad uses this stick as directions. Keep center noise
     * neutral; never rebase a held direction as the center of the device. */
    if (value>=-8192 && value<=8192) return 0;
    return value;
}

/* Retain the core's advertised maximum, rounded independently per axis.
 * This removes unused square padding without reducing emulation resolution.
 * Refuse malformed/oversized geometry rather than silently clipping it. */
static inline bool magicx_saturn_target(unsigned max_width,unsigned max_height,
        unsigned *width,unsigned *height)
{
    unsigned w=1,h=1;
    if (!max_width || !max_height || max_width>*width || max_height>*height)
        return false;
    while (w<max_width) w*=2;
    while (h<max_height) h*=2;
    if (w>*width || h>*height) return false;
    *width=w;*height=h;
    return true;
}
/* GLES3/full-NPOT contexts do not need power-of-two padding. Retain every
 * pixel of the core's advertised maximum; unsupported contexts use the
 * existing rounded allocation above. */
static inline bool magicx_saturn_exact_target(unsigned max_width,unsigned max_height,
        unsigned *width,unsigned *height)
{
    if (!max_width || !max_height || max_width>*width || max_height>*height)
        return false;
    *width=max_width;*height=max_height;
    return true;
}
struct magicx_saturn_phases {
    uint64_t total[4],maximum[4];
    unsigned count;
};
/* CPU wall time around existing calls, not GPU timestamp measurements. */
static inline bool magicx_saturn_phases_add(struct magicx_saturn_phases *p,
        const uint64_t marks[5])
{
    unsigned i;
    if (!marks[0]) return false;
    for (i=0;i<4;i++) if (marks[i+1]<marks[i]) return false;
    for (i=0;i<4;i++) {
        uint64_t elapsed=marks[i+1]-marks[i];
        p->total[i]+=elapsed;
        if (elapsed>p->maximum[i]) p->maximum[i]=elapsed;
    }
    return ++p->count==300;
}
#endif
