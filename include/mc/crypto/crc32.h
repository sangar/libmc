#pragma once

#include <stddef.h>
#include <stdint.h>

// crc32_update continues the IEEE CRC-32 of zlib, gzip and PNG over data;
// start from 0.
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);
// crc32c_update continues the Castagnoli CRC-32 of iSCSI, ext4 and S3's
// x-amz-checksum-crc32c over data; start from 0.
uint32_t crc32c_update(uint32_t crc, const void *data, size_t len);
