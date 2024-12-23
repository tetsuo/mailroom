#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include "log.h"

static void get_timestamp(char *buffer, size_t size)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts); // Use CLOCK_REALTIME for the current time

  struct tm *local_time = localtime(&ts.tv_sec); // Convert seconds to local time
  if (local_time)
  {
    snprintf(buffer, size, "%04d/%02d/%02d %02d:%02d:%02d.%03ld",
             local_time->tm_year + 1900,
             local_time->tm_mon + 1,
             local_time->tm_mday,
             local_time->tm_hour,
             local_time->tm_min,
             local_time->tm_sec,
             ts.tv_nsec / 1000000); // Convert nanoseconds to milliseconds
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
  char timestamp[25];
  get_timestamp(timestamp, sizeof(timestamp));
  fprintf(stderr, "%s  ", timestamp);
  vfprintf(stderr, format, args);
  fprintf(stderr, "\n");
  va_end(args);
}
