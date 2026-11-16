/* SPDX-License-Identifier: GPL-3.0-only
 * Copied from C5VRX p4usb (reference/p4usb/main/yuv420.S), by Twotoz and the C5VRX contributors. */
/* void yuv420_rows(const uint8_t *in, uint8_t *with_v, uint8_t *with_u)
 *
 * One 720-pixel row of V Y0 U Y1 pairs as the H.264 encoder's packed 4:2:0, C Y0 Y1 a pair, twice: once
 * with each pair's V for an even row and once with its U for an odd one. 22 blocks of 16 pairs, then 8.
 * in 16-byte aligned, outputs 8-byte aligned. */
    .text
    .align 4
    .global yuv420_rows
    .type yuv420_rows, @function
yuv420_rows:
    li t0, 22
1:
    esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a0, 16
    esp.vld.128.ip q2, a0, 16
    esp.vld.128.ip q3, a0, 16
    /* Chroma and luma bytes apart, then V from U and Y0 from Y1. */
    esp.vunzip.8 q0, q1
    esp.vunzip.8 q2, q3
    esp.vunzip.8 q0, q2
    esp.vunzip.8 q1, q3
    /* The three-way zip works in place, so the second row gets copies of the Ys. */
    esp.orq q4, q1, q1
    esp.orq q5, q3, q3
    esp.vzipt.8 q0, q1, q3
    esp.vzipt.8 q2, q4, q5
    esp.vst.l.64.ip q0, a1, 8
    esp.vst.h.64.ip q0, a1, 8
    esp.vst.l.64.ip q1, a1, 8
    esp.vst.h.64.ip q1, a1, 8
    esp.vst.l.64.ip q3, a1, 8
    esp.vst.h.64.ip q3, a1, 8
    esp.vst.l.64.ip q2, a2, 8
    esp.vst.h.64.ip q2, a2, 8
    esp.vst.l.64.ip q4, a2, 8
    esp.vst.h.64.ip q4, a2, 8
    esp.vst.l.64.ip q5, a2, 8
    esp.vst.h.64.ip q5, a2, 8
    addi t0, t0, -1
    bnez t0, 1b
    /* The last 8 pairs: zeros stand in for the missing half and are not stored. */
    esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a0, 16
    esp.vunzip.8 q0, q1
    esp.zero.q q2
    esp.vunzip.8 q0, q2
    esp.zero.q q3
    esp.vunzip.8 q1, q3
    esp.orq q4, q1, q1
    esp.orq q5, q3, q3
    esp.vzipt.8 q0, q1, q3
    esp.vzipt.8 q2, q4, q5
    esp.vst.l.64.ip q0, a1, 8
    esp.vst.h.64.ip q0, a1, 8
    esp.vst.l.64.ip q1, a1, 8
    esp.vst.l.64.ip q2, a2, 8
    esp.vst.h.64.ip q2, a2, 8
    esp.vst.l.64.ip q4, a2, 8
    ret
    .size yuv420_rows, .-yuv420_rows
