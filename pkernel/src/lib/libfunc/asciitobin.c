#include "func.h"
#include "string.h"

void asciitobin(char* _str, size_t size)
{
	int i;
	char tmp[3] = {0,};

	for(i = 0; i < size; i += 2)
	{
		tmp[0] = _str[i];
		tmp[1] = _str[i + 1];

		_str[i / 2] = (char)decto10(tmp);
	}

	memset(&_str[size / 2], 0x00, size / 2);
}
