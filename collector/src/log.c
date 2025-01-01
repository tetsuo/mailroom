#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static void get_timestamp_utc(char *buffer, size_t size)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);

  struct tm *utc_time = gmtime(&ts.tv_sec);
  if (utc_time)
  {
    snprintf(buffer, size, "%04d/%02d/%02d %02d:%02d:%02d",
             utc_time->tm_year + 1900,
             utc_time->tm_mon + 1,
             utc_time->tm_mday,
             utc_time->tm_hour,
             utc_time->tm_min,
             utc_time->tm_sec);
  }
  else
  {
    snprintf(buffer, size, "unknown-time");
  }
}

void log_printf(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  char timestamp[20];
  get_timestamp_utc(timestamp, sizeof(timestamp));
  fprintf(stderr, "%s [PG] ", timestamp);
  vfprintf(stderr, format, args);
  fprintf(stderr, "\n");
  va_end(args);
}
