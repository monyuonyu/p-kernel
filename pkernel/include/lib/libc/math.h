#ifndef _MATH_H_
#define _MATH_H_

double acos(double x);
double asin(double x);
double atan(double x);
double atan2(double y, double x);
double cos(double x);
double sin(double x);
double tan(double x);
double cosh(double x);
double sinh(double x);
double tanh(double x);
double exp(double x);
double frexp(double value, int *exp);
double ldexp(double x, int exp);
double log(double x);
double log10(double x);
double modf(double value, double *iptr);
double pow(double x, double y);
double sqrt(double x);
double ceil(double x);
double fabs(double x);
double floor(double x);
double fmod(double x, double y);

float acosf(float x);
float asinf(float x);
float atanf(float x);
float atan2f(float y, float x);
float cosf(float x);
float sinf(float x);
float tanf(float x);
float coshf(float x);
float sinhf(float x);
float tanhf(float x);
float expf(float x);
float frexpf(float value, int *exp);
float ldexpf(float x, int exp);
float logf(float x);
float log10f(float x);
float modff(float value, float *iptr);
float powf(float x, float y);
float sqrtf(float x);
float ceilf(float x);
float fabsf(float x);
float floorf(float x);
float fmodf(float x, float y);

#endif  /* _MATH_H_ */
