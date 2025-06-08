#include "stdlib.h"
#include "string.h"

static void swap(void* a, void* b, size_t size)
{
	char* ca = (char*)a;
	char* cb = (char*)b;
	char temp;
	
	{
	size_t i;
	for (i = 0; i < size; i++) {
		temp = ca[i];
		ca[i] = cb[i];
		cb[i] = temp;
	}
	}
}

static void* partition(void* base, size_t nmemb, size_t size,
                      int (*compar)(const void*, const void*))
{
	char* cbase = (char*)base;
	char* pivot = cbase + (nmemb - 1) * size;
	size_t i = 0;
	
	{
	size_t j;
	for (j = 0; j < nmemb - 1; j++) {
		if (compar(cbase + j * size, pivot) <= 0) {
			swap(cbase + i * size, cbase + j * size, size);
			i++;
		}
	}
	}
	
	swap(cbase + i * size, pivot, size);
	return cbase + i * size;
}

void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*))
{
	if (nmemb <= 1)
		return;
	
	char* cbase = (char*)base;
	char* pivot = (char*)partition(base, nmemb, size, compar);
	size_t pivot_idx = (pivot - cbase) / size;
	
	qsort(base, pivot_idx, size, compar);
	qsort(pivot + size, nmemb - pivot_idx - 1, size, compar);
}