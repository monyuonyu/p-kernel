#include "string.h"

char* strstr(const char* haystack, const char* needle)
{
	size_t needle_len = strlen(needle);
	
	if (needle_len == 0)
		return (char*)haystack;
	
	while (*haystack) {
		if (strncmp(haystack, needle, needle_len) == 0)
			return (char*)haystack;
		haystack++;
	}
	
	return NULL;
}