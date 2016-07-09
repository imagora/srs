/*
 The MIT License (MIT)
 
 Copyright (c) 2013-2015 SRS(ossrs)
 
 Permission is hereby granted, free of charge, to any person obtaining a copy of
 this software and associated documentation files (the "Software"), to deal in
 the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "srs_lib_log.hpp"
#include <srs_kernel_error.hpp>
#include <syslog.h>
#include <stdarg.h>


SrsSysLog::SrsSysLog()
{
    initialize();
}


SrsSysLog::~SrsSysLog()
{
    ::closelog();
}


int SrsSysLog::initialize()
{
    ::openlog(nullptr, LOG_PID|LOG_NDELAY, LOG_USER|LOG_DAEMON);
    return ERROR_SUCCESS;
}


void SrsSysLog::verbose(const char* tag, int context_id, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(LOG_DEBUG, fmt, args);
    va_end(args);
}


void SrsSysLog::info(const char* tag, int context_id, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(LOG_INFO, fmt, args);
    va_end(args);
}


void SrsSysLog::trace(const char* tag, int context_id, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(LOG_NOTICE, fmt, args);
    va_end(args);
}


void SrsSysLog::warn(const char* tag, int context_id, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(LOG_WARNING, fmt, args);
    va_end(args);
}


void SrsSysLog::error(const char* tag, int context_id, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(LOG_ERR, fmt, args);
    va_end(args);
}


void SrsSysLog::log(int level, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    ::vsyslog(level, format, args);
    va_end(args);
}
