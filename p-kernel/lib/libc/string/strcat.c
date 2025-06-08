#include "string.h"

char* strcat(char* dst, const char* src)
{
	char* d = dst;
	while (*d)
		d++;
	while ((*d++ = *src++))
		;
	return dst;
}