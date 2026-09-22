/*
 * tinfl.h - Tiny Infflate Header
 * Copyright (c) 2003-2019 Joergen Ibsen
 * zlib license
 */
#ifndef TINF_H_INCLUDED
#define TINF_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TINF_OK             0
#define TINF_DATA_ERROR    (-3)
#define TINF_BUF_ERROR     (-5)

int tinf_zlib_uncompress(void *dest, unsigned int *destLen,
                         const void *source, unsigned int sourceLen);

#ifdef __cplusplus
}
#endif

#endif /* TINF_H_INCLUDED */
