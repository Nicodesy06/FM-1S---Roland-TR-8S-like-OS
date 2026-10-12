/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 PULSAR-8 contributors */
/* 64-bit division for the freestanding pi32v2 build (no compiler runtime is linked). The hot paths
 * divide in 32 bits; these serve the rare ones (tempo, MIDI clock, delay time, event offsets). */
#include <stdint.h>
uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    int i;
    if (d == 0) {
        if (rem)
            *rem = 0;
        return ~(uint64_t)0;
    }
    if ((n >> 32) == 0 && (d >> 32) == 0) {
        uint32_t a = (uint32_t)n, b = (uint32_t)d;
        if (rem)
            *rem = a % b;
        return a / b;
    }
    for (i = 63; i >= 0; i--) {
        r = r << 1 | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d) { return __udivmoddi4(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    __udivmoddi4(n, d, &r);
    return r;
}
int64_t __divdi3(int64_t a, int64_t b)
{
    int neg = (a < 0) != (b < 0);
    uint64_t q = __udivmoddi4(a < 0 ? (uint64_t)-a : (uint64_t)a, b < 0 ? (uint64_t)-b : (uint64_t)b, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}
int64_t __moddi3(int64_t a, int64_t b)
{
    uint64_t r;
    __udivmoddi4(a < 0 ? (uint64_t)-a : (uint64_t)a, b < 0 ? (uint64_t)-b : (uint64_t)b, &r);
    return a < 0 ? -(int64_t)r : (int64_t)r;
}
