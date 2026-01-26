#include "crc16_ccitt.h"

uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t seed)
{
    uint16_t crc = seed; // common seed: 0xFFFF
    const uint16_t poly = 0x1021;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
        {
            if (crc & 0x8000) crc = (crc << 1) ^ poly;
            else             crc = (crc << 1);
        }
    }
    return crc;
}
