#ifndef SNOWZIP_H
#define SNOWZIP_H

#include <stdint.h>
#include <stddef.h>

#define SNZ_MAGIC "SNZIP02\0"
#define SNZ_MAGIC_SIZE 8

#define SNZ_METHOD_STORE   0
#define SNZ_METHOD_DEFLATE 1

uint32_t snowzip_crc32(const void *data, size_t size);

#endif
