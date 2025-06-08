#include "string.h"

void* memchr(const void* s, int c, size_t n)
{
	const unsigned char* p = (const unsigned char*)s;
	unsigned char ch = (unsigned char)c;
	
	while (n--) {
		if (*p == ch)
			return (void*)p;
		p++;
	}
	
	return NULL;
}