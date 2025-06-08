#include "stdlib.h"
#include "ctype.h"

unsigned long int strtoul(const char* str, char** endptr, int base)
{
	const char* start = str;
	unsigned long int result = 0;
	int digit;
	
	while (isspace(*str))
		str++;
	
	if (base == 0) {
		if (*str == '0') {
			str++;
			if (*str == 'x' || *str == 'X') {
				str++;
				base = 16;
			} else {
				base = 8;
				str--;
			}
		} else {
			base = 10;
		}
	} else if (base == 16) {
		if (*str == '0' && (str[1] == 'x' || str[1] == 'X'))
			str += 2;
	}
	
	if (base < 2 || base > 36) {
		if (endptr)
			*endptr = (char*)start;
		return 0;
	}
	
	while (*str) {
		if (isdigit(*str))
			digit = *str - '0';
		else if (isalpha(*str))
			digit = tolower(*str) - 'a' + 10;
		else
			break;
		
		if (digit >= base)
			break;
		
		result = result * base + digit;
		str++;
	}
	
	if (endptr)
		*endptr = (char*)str;
	
	return result;
}