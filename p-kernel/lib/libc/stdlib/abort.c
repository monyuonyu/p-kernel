#include "stdlib.h"

void abort(void)
{
	while (1) {
		__asm__ volatile("nop");
	}
}