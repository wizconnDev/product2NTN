#ifndef EXTFLASH_W25Q16_H
#define EXTFLASH_W25Q16_H

#include "stm32l0xx_hal.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SPI_HandleTypeDef *hspi;
    GPIO_TypeDef *cs_port;
    uint16_t cs_pin;
} ExtFlash;

void     ExtFlash_Init(ExtFlash *f, SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin);

uint8_t  ExtFlash_ReadSR1(ExtFlash *f);
int      ExtFlash_WaitReady(ExtFlash *f, uint32_t timeout_ms);

int      ExtFlash_ReadJedecId(ExtFlash *f, uint8_t id3[3]);

int      ExtFlash_Read(ExtFlash *f, uint32_t addr, uint8_t *buf, size_t len);

int      ExtFlash_Erase4K(ExtFlash *f, uint32_t addr_4k_aligned, uint32_t timeout_ms);
int      ExtFlash_Write(ExtFlash *f, uint32_t addr, const uint8_t *buf, size_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
