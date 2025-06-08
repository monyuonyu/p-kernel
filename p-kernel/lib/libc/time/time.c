#include "time.h"

static time_t system_time = 0;
static clock_t system_clock = 0;

clock_t clock(void)
{
	return system_clock++;
}

time_t time(time_t* tloc)
{
	system_time++;
	if (tloc)
		*tloc = system_time;
	return system_time;
}

double difftime(time_t time1, time_t time0)
{
	return (double)(time1 - time0);
}