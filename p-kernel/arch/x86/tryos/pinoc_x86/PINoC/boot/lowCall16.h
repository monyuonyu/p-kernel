#ifndef __lowCall16__
#define __lowCall16__

void io_printf16(s1 _Src[][8]);
void io_RGBwrite16(si xsize, si ysize, si xSrc, si ySrc, RGB rgb);
void io_graphic16(u1 mode);
void io_sd16(SD* _sd, ui base, ui limit, u1 a, u1 type, u1 s, u1 dpl, u1 p, u1 avl, u1 db, u1 g);
void LGDT16(u2 Limit, u4 Base);
void LIDT16(u2 Limit, u4 Base);

#endif
