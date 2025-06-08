#include "math.h"

double sqrt(double x)
{
	if (x < 0.0)
		return 0.0;
	
	if (x == 0.0)
		return 0.0;
	
	double guess = x / 2.0;
	double prev_guess = 0.0;
	
	while (fabs(guess - prev_guess) > 1e-10) {
		prev_guess = guess;
		guess = (guess + x / guess) / 2.0;
	}
	
	return guess;
}

float sqrtf(float x)
{
	if (x < 0.0f)
		return 0.0f;
	
	if (x == 0.0f)
		return 0.0f;
	
	float guess = x / 2.0f;
	float prev_guess = 0.0f;
	
	while (fabsf(guess - prev_guess) > 1e-6f) {
		prev_guess = guess;
		guess = (guess + x / guess) / 2.0f;
	}
	
	return guess;
}