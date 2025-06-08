#include "math.h"

double sinh(double x)
{
	return (exp(x) - exp(-x)) / 2.0;
}

double cosh(double x)
{
	return (exp(x) + exp(-x)) / 2.0;
}

double tanh(double x)
{
	double exp_x = exp(x);
	double exp_neg_x = exp(-x);
	return (exp_x - exp_neg_x) / (exp_x + exp_neg_x);
}

float sinhf(float x) { return (float)sinh((double)x); }
float coshf(float x) { return (float)cosh((double)x); }
float tanhf(float x) { return (float)tanh((double)x); }