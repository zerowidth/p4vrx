/* void deint_row(uint8_t *out, const uint8_t *above, const uint8_t *below, const uint8_t *old, const uint8_t *chroma, const uint8_t *masks)
 *
 * One 1080-byte row of packed 4:2:0 (C Y0 Y1 a pixel pair): each Y is the median of the Ys above, below
 * and in the previous field, and each C is chroma's. masks is three 16-byte Y masks then three chroma
 * masks, for the three blocks the 3-byte pattern takes to repeat. 67 blocks and a half; all pointers
 * 16-byte aligned, and inputs are read one half block past the end. */
    .text
    .align 4
    .global deint_row
    .type deint_row, @function
deint_row:
    li t0, 22
1:
    li t2, 3
2:
    esp.vld.128.ip q0, a1, 16
    esp.vld.128.ip q1, a2, 16
    esp.vld.128.ip q2, a3, 16
    esp.vld.128.ip q3, a4, 16
    esp.vld.128.ip q6, a5, 48
    esp.vld.128.ip q7, a5, -32
    esp.vmin.u8 q4, q0, q1
    esp.vmax.u8 q5, q0, q1
    esp.vmin.u8 q5, q5, q2
    esp.vmax.u8 q4, q4, q5
    esp.andq q4, q4, q6
    esp.andq q3, q3, q7
    esp.orq q4, q4, q3
    esp.vst.128.ip q4, a0, 16
    addi t2, t2, -1
    bnez t2, 2b
    /* Back to the first masks. */
    addi a5, a5, -48
    addi t0, t0, -1
    bnez t0, 1b
    /* Block 66 takes the first masks, and of block 67 only the low half is the row's. */
    esp.vld.128.ip q0, a1, 16
    esp.vld.128.ip q1, a2, 16
    esp.vld.128.ip q2, a3, 16
    esp.vld.128.ip q3, a4, 16
    esp.vld.128.ip q6, a5, 48
    esp.vld.128.ip q7, a5, -32
    esp.vmin.u8 q4, q0, q1
    esp.vmax.u8 q5, q0, q1
    esp.vmin.u8 q5, q5, q2
    esp.vmax.u8 q4, q4, q5
    esp.andq q4, q4, q6
    esp.andq q3, q3, q7
    esp.orq q4, q4, q3
    esp.vst.128.ip q4, a0, 16
    esp.vld.128.ip q0, a1, 16
    esp.vld.128.ip q1, a2, 16
    esp.vld.128.ip q2, a3, 16
    esp.vld.128.ip q3, a4, 16
    esp.vld.128.ip q6, a5, 48
    esp.vld.128.ip q7, a5, -32
    esp.vmin.u8 q4, q0, q1
    esp.vmax.u8 q5, q0, q1
    esp.vmin.u8 q5, q5, q2
    esp.vmax.u8 q4, q4, q5
    esp.andq q4, q4, q6
    esp.andq q3, q3, q7
    esp.orq q4, q4, q3
    esp.vst.l.64.ip q4, a0, 8
    ret
    .size deint_row, .-deint_row
