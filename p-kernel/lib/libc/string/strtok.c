#include "string.h"

static char* last_token = NULL;

char* strtok(char* str, const char* delim)
{
	char* token_start;
	char* token_end;
	
	if (str != NULL)
		last_token = str;
	else if (last_token == NULL)
		return NULL;
	
	token_start = last_token + strspn(last_token, delim);
	
	if (*token_start == '\0') {
		last_token = NULL;
		return NULL;
	}
	
	token_end = strpbrk(token_start, delim);
	
	if (token_end == NULL) {
		last_token = NULL;
	} else {
		*token_end = '\0';
		last_token = token_end + 1;
	}
	
	return token_start;
}