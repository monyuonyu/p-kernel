#ifndef _TIME_H_
#define _TIME_H_

#include "stddef.h"

typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 1000000L

struct tm {
	int tm_sec;    /* seconds (0-60) */
	int tm_min;    /* minutes (0-59) */
	int tm_hour;   /* hours (0-23) */
	int tm_mday;   /* day of the month (1-31) */
	int tm_mon;    /* month (0-11) */
	int tm_year;   /* year - 1900 */
	int tm_wday;   /* day of the week (0-6, Sunday = 0) */
	int tm_yday;   /* day in the year (0-365, 1 Jan = 0) */
	int tm_isdst;  /* daylight saving time */
};

clock_t clock(void);
time_t time(time_t* tloc);
double difftime(time_t time1, time_t time0);
time_t mktime(struct tm* timeptr);

char* asctime(const struct tm* timeptr);
char* ctime(const time_t* timer);
struct tm* gmtime(const time_t* timer);
struct tm* localtime(const time_t* timer);
size_t strftime(char* s, size_t maxsize, const char* format, const struct tm* timeptr);

#endif /* _TIME_H_ */