#include "string.h"

char* strncpy(char* dst, const char* src, size_t n)
{
	char* d = dst;
	while (n && (*d++ = *src++))
		n--;
	while (n--)
		*d++ = '\0';
	return dst;
}