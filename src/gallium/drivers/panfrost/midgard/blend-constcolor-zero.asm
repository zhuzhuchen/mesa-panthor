ld_color_buffer_8 r1.x, 0.xxxx, 0x0

ld_color_buffer_8 r1.y, 0.xxxx, 0x1

ld_color_buffer_8 r1.z, 0.xxxx, 0x2

ld_color_buffer_8 r1.w, 0.xxxx, 0x3

vadd.u2f hr2.xyzw, abs(hr2), #0

vadd.f2u8.pos.low hr0, hr2, #0

vmul.imov.quarter r0, r0, r0
br.write.always +0 -> 8

vmul.imov.quarter r0, r0, r0
br.write.always -1 -> 8
