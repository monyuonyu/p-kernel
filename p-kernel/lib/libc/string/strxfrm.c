#include "string.h"

size_t strxfrm(char* dst, const char* src, size_t n)
{
	size_t len = strlen(src);
	
	if (n > 0) {
		if (len < n) {
			strcpy(dst, src);
		} else {
			strncpy(dst, src, n - 1);
			dst[n - 1] = '\0';
		}
	}
	
	return len;
}