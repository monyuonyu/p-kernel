#include "stdlib.h"

void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*))
{
	const char* cbase = (const char*)base;
	size_t left = 0;
	size_t right = nmemb;
	
	while (left < right) {
		size_t mid = left + (right - left) / 2;
		const void* mid_elem = cbase + mid * size;
		int cmp = compar(key, mid_elem);
		
		if (cmp == 0)
			return (void*)mid_elem;
		else if (cmp < 0)
			right = mid;
		else
			left = mid + 1;
	}
	
	return NULL;
}