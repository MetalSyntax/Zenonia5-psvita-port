/**
 * @file darwin.h
 * @brief Darwin POSIX/libc/pthreads subsystem bridge for PS Vita.
 */
#pragma once
#include <stddef.h>

void darwin_init(void);
const char *darwin_path(const char *in, char *out, size_t outlen);
extern volatile unsigned darwin_io_ops; // fopen del juego: el watchdog lo usa como señal de carga en curso
extern char darwin_last_open[256];       // ruta del último fopen del juego
