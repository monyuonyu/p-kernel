#include "string.h"

char* strrchr(const char* s, int c)
{
	const char* last = NULL;
	
	while (*s) {
		if (*s == c)
			last = s;
		s++;
	}
	
	if (c == '\0')
		return (char*)s;
	
	return (char*)last;
}