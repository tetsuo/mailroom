#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static void get_timestamp(char *buffer, size_t size)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);

  struct tm *local_time = localtime(&ts.tv_sec);
  if (local_time)
  {
    snprintf(buffer, size, "%04d/%02d/%02d %02d:%02d:%02d",
             local_time->tm_year + 1900,
             local_time->tm_mon + 1,
             local_time->tm_mday,
             local_time->tm_hour,
             local_time->tm_min,
             local_time->tm_sec);
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
  get_timestamp(timestamp, sizeof(timestamp));
  fprintf(stderr, "%s ", timestamp);
  vfprintf(stderr, format, args);
  fprintf(stderr, "\n");
  va_end(args);
}
