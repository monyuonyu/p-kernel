#include "stdlib.h"
#include "ctype.h"

double strtod(const char* str, char** endptr)
{
	const char* start = str;
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
	
	int has_digits = 0;
	while (isdigit(*str)) {
		result = result * 10.0 + (*str - '0');
		str++;
		has_digits = 1;
	}
	
	if (*str == '.') {
		str++;
		while (isdigit(*str)) {
			fraction = fraction * 10.0 + (*str - '0');
			divisor *= 10.0;
			str++;
			has_digits = 1;
		}
	}
	
	if (!has_digits) {
		if (endptr)
			*endptr = (char*)start;
		return 0.0;
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
	
	if (endptr)
		*endptr = (char*)str;
	
	return sign * result;
}