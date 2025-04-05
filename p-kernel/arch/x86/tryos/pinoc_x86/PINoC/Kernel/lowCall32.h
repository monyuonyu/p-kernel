#ifndef __LOWCALL32__
#define __LOWCALL32__

void io_fin32();
void io_hlt32();
void io_out8(int port, int data);
void io_write32(si xsize, si ysize, si xSrc, si ySrc, s1 color);
void io_sd32(SD* _sd, ui base, ui limit, u1 a, u1 type, u1 s, u1 dpl, u1 p, u1 avl, u1 db, u1 g);
void init_pic();

#define PIC0_ICW1               0x0020
#define PIC0_OCW2               0x0020
#define PIC0_IMR                0x0021
#define PIC0_ICW2               0x0021
#define PIC0_ICW3               0x0021
#define PIC0_ICW4               0x0021
#define PIC1_ICW1               0x00a0
#define PIC1_OCW2               0x00a0
#define PIC1_IMR                0x00a1
#define PIC1_ICW2               0x00a1
#define PIC1_ICW3               0x00a1
#define PIC1_ICW4               0x00a1

struct GATE_DIS
{
	short offset_low;
	short selecta;
	struct
	{
		short reserve : 8;
		short type : 4;
		short s : 1;
		short dpl : 2;
		short p : 1;
	};
	short offset_high;
};


#endif
