#include "math.h"

double floor(double x)
{
	if (x >= 0.0) {
		return (double)((long)x);
	} else {
		long i = (long)x;
		return (x == (double)i) ? x : (double)(i - 1);
	}
}

double ceil(double x)
{
	if (x >= 0.0) {
		long i = (long)x;
		return (x == (double)i) ? x : (double)(i + 1);
	} else {
		return (double)((long)x);
	}
}

float floorf(float x)
{
	if (x >= 0.0f) {
		return (float)((long)x);
	} else {
		long i = (long)x;
		return (x == (float)i) ? x : (float)(i - 1);
	}
}

float ceilf(float x)
{
	if (x >= 0.0f) {
		long i = (long)x;
		return (x == (float)i) ? x : (float)(i + 1);
	} else {
		return (float)((long)x);
	}
}