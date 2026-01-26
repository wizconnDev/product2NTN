#ifndef CRC16_CCITT_H
#define CRC16_CCITT_H

#include <stdint.h>
#include <stddef.h>

uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t seed);

#endif
