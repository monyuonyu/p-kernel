#include "time.h"

static int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static int is_leap_year(int year)
{
	return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

static int days_since_epoch(int year, int month, int day)
{
	int days = 0;
	int y;
	
	for (y = 1970; y < year; y++) {
		days += is_leap_year(y) ? 366 : 365;
	}
	
	{
	int m;
	for (m = 0; m < month; m++) {
		days += days_in_month[m];
		if (m == 1 && is_leap_year(year))
			days++;
	}
	}
	
	days += day - 1;
	
	return days;
}

time_t mktime(struct tm* timeptr)
{
	int year = timeptr->tm_year + 1900;
	int month = timeptr->tm_mon;
	int day = timeptr->tm_mday;
	
	int days = days_since_epoch(year, month, day);
	time_t seconds = (time_t)days * 24 * 60 * 60;
	seconds += timeptr->tm_hour * 60 * 60;
	seconds += timeptr->tm_min * 60;
	seconds += timeptr->tm_sec;
	
	return seconds;
}