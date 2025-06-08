#include "stdlib.h"

void exit(int status)
{
	(void)status;
	while (1) {
		__asm__ volatile("nop");
	}
}