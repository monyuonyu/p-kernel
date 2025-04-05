#include "string.h"

void *memcpy(void * s1, const void * s2, size_t n)
{
	register unsigned char* _s1 = (unsigned char *)s1;
	register unsigned char* _s2 = (unsigned char *)s2;

	int cnt;
	for(cnt = 0; cnt < n; cnt++)
	{
		*_s1++ = *_s2++;
	}

	return s1;
}
