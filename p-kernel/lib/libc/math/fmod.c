#include "math.h"

double fmod(double x, double y)
{
	if (y == 0.0) return 0.0;
	
	double quotient = x / y;
	double integer_part = floor(quotient);
	
	return x - integer_part * y;
}

float fmodf(float x, float y)
{
	return (float)fmod((double)x, (double)y);
}