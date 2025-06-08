#include "stdlib.h"
#include "ctype.h"

double atof(const char* str)
{
	double result = 0.0;
	double fraction = 0.0;
	int sign = 1;
	int exp_sign = 1;
	int exp = 0;
	double divisor = 1.0;
	
	while (isspace(*str))
		str++;
	
	if (*str == '-') {
		sign = -1;
		str++;
	} else if (*str == '+') {
		str++;
	}
	
	while (isdigit(*str)) {
		result = result * 10.0 + (*str - '0');
		str++;
	}
	
	if (*str == '.') {
		str++;
		while (isdigit(*str)) {
			fraction = fraction * 10.0 + (*str - '0');
			divisor *= 10.0;
			str++;
		}
	}
	
	result = result + fraction / divisor;
	
	if (*str == 'e' || *str == 'E') {
		str++;
		if (*str == '-') {
			exp_sign = -1;
			str++;
		} else if (*str == '+') {
			str++;
		}
		
		while (isdigit(*str)) {
			exp = exp * 10 + (*str - '0');
			str++;
		}
		
		{
		int i;
		for (i = 0; i < exp; i++) {
			if (exp_sign == 1)
				result *= 10.0;
			else
				result /= 10.0;
		}
		}
	}
	
	return sign * result;
}