#include "string.h"

char* strpbrk(const char* s1, const char* s2)
{
	const char* p2;
	
	while (*s1) {
		for (p2 = s2; *p2; p2++) {
			if (*s1 == *p2)
				return (char*)s1;
		}
		s1++;
	}
	
	return NULL;
}