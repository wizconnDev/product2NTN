#include "extflash_w25q16.h"
#include "string.h"

/* W25Q command set (common) */
#define CMD_READ_DATA           0x03
#define CMD_PAGE_PROGRAM        0x02
#define CMD_WRITE_ENABLE        0x06
#define CMD_READ_SR1            0x05
#define CMD_SECTOR_ERASE_4K     0x20
#define CMD_JEDEC_ID            0x9F
#define CMD_RELEASE_POWERDOWN   0xAB

/* SR1 bits */
#define SR1_WIP                 (1u << 0)  // Write In Progress
#define SR1_WEL                 (1u << 1)  // Write Enable Latch

#define W25Q_PAGE_SIZE          256u
#define W25Q_SECTOR_SIZE_4K     4096u

static inline void CS_L(ExtFlash *f) { HAL_GPIO_WritePin(f->cs_port, f->cs_pin, GPIO_PIN_RESET); }
static inline void CS_H(ExtFlash *f) { HAL_GPIO_WritePin(f->cs_port, f->cs_pin, GPIO_PIN_SET); }

void ExtFlash_Init(ExtFlash *f, SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin)
{
    f->hspi = hspi;
    f->cs_port = cs_port;
    f->cs_pin = cs_pin;

    CS_H(f);

    /* Some boards keep flash in deep power-down after VBAT domain events.
       Safe to send Release-from-PD command (0xAB). */
    uint8_t cmd = CMD_RELEASE_POWERDOWN;
    CS_L(f);
    (void)HAL_SPI_Transmit(f->hspi, &cmd, 1, 100);
    CS_H(f);
    HAL_Delay(1);
}

uint8_t ExtFlash_ReadSR1(ExtFlash *f)
{
    uint8_t cmd = CMD_READ_SR1;
    uint8_t sr1 = 0xFF;

    CS_L(f);
    (void)HAL_SPI_Transmit(f->hspi, &cmd, 1, 100);
    (void)HAL_SPI_Receive(f->hspi, &sr1, 1, 100);
    CS_H(f);
    return sr1;
}

static int WriteEnable(ExtFlash *f)
{
    uint8_t cmd = CMD_WRITE_ENABLE;
    CS_L(f);
    if (HAL_SPI_Transmit(f->hspi, &cmd, 1, 100) != HAL_OK) { CS_H(f); return 0; }
    CS_H(f);

    /* Check WEL */
    uint8_t sr1 = ExtFlash_ReadSR1(f);
    return (sr1 & SR1_WEL) ? 1 : 0;
}
//相当于 HAL_Delay(500);的类似等待
int ExtFlash_WaitReady(ExtFlash *f, uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < timeout_ms)
    {
        uint8_t sr1 = ExtFlash_ReadSR1(f);
        if ((sr1 & SR1_WIP) == 0) return 1; // ready
        HAL_Delay(1);
    }
    return 0; // timeout
}

//Probe / Identify（识别芯片）
int ExtFlash_ReadJedecId(ExtFlash *f, uint8_t id3[3])
{
    uint8_t cmd = CMD_JEDEC_ID;
    uint8_t rx[3] = {0};

    CS_L(f);
    if (HAL_SPI_Transmit(f->hspi, &cmd, 1, 100) != HAL_OK) { CS_H(f); return 0; }
    if (HAL_SPI_Receive(f->hspi, rx, 3, 100) != HAL_OK) { CS_H(f); return 0; }
    CS_H(f);

    id3[0] = rx[0];
    id3[1] = rx[1];
    id3[2] = rx[2];
    return 1;
}

int ExtFlash_Read(ExtFlash *f, uint32_t addr, uint8_t *buf, size_t len)
{
    uint8_t hdr[4];
    hdr[0] = CMD_READ_DATA;
    hdr[1] = (uint8_t)(addr >> 16);
    hdr[2] = (uint8_t)(addr >> 8);
    hdr[3] = (uint8_t)(addr);

    CS_L(f);
    if (HAL_SPI_Transmit(f->hspi, hdr, 4, 200) != HAL_OK) { CS_H(f); return 0; }
    if (HAL_SPI_Receive(f->hspi, buf, (uint16_t)len, 2000) != HAL_OK) { CS_H(f); return 0; }
    CS_H(f);
    return 1;
}

int ExtFlash_Erase4K(ExtFlash *f, uint32_t addr_4k_aligned, uint32_t timeout_ms)
{
    if (addr_4k_aligned % W25Q_SECTOR_SIZE_4K != 0) return 0;
    if (!ExtFlash_WaitReady(f, 2000)) return 0;
    if (!WriteEnable(f)) return 0;

    uint8_t hdr[4];
    hdr[0] = CMD_SECTOR_ERASE_4K;
    hdr[1] = (uint8_t)(addr_4k_aligned >> 16);
    hdr[2] = (uint8_t)(addr_4k_aligned >> 8);
    hdr[3] = (uint8_t)(addr_4k_aligned);

    CS_L(f);
    if (HAL_SPI_Transmit(f->hspi, hdr, 4, 200) != HAL_OK) { CS_H(f); return 0; }
    CS_H(f);

    return ExtFlash_WaitReady(f, timeout_ms);
}

static int PageProgram(ExtFlash *f, uint32_t addr, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (len == 0 || len > W25Q_PAGE_SIZE) return 0;
    if (!ExtFlash_WaitReady(f, 2000)) return 0;
    if (!WriteEnable(f)) return 0;

    uint8_t hdr[4];
    hdr[0] = CMD_PAGE_PROGRAM;
    hdr[1] = (uint8_t)(addr >> 16);
    hdr[2] = (uint8_t)(addr >> 8);
    hdr[3] = (uint8_t)(addr);

    CS_L(f);
    if (HAL_SPI_Transmit(f->hspi, hdr, 4, 200) != HAL_OK) { CS_H(f); return 0; }
    if (HAL_SPI_Transmit(f->hspi, (uint8_t*)data, (uint16_t)len, 2000) != HAL_OK) { CS_H(f); return 0; }
    CS_H(f);

    return ExtFlash_WaitReady(f, timeout_ms);
}

int ExtFlash_Write(ExtFlash *f, uint32_t addr, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    /* Split into page-sized writes, respecting page boundaries */
    size_t off = 0;
    while (off < len)
    {
        uint32_t cur = addr + (uint32_t)off;
        uint32_t page_off = cur % W25Q_PAGE_SIZE;
        size_t chunk = W25Q_PAGE_SIZE - page_off;
        if (chunk > (len - off)) chunk = (len - off);

        if (!PageProgram(f, cur, buf + off, chunk, timeout_ms))
            return 0;

        off += chunk;
    }
    return 1;
}
