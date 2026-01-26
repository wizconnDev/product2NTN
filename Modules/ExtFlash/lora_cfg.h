#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_CFG_MAGIC  (0x41524F4Cu)   // 'LORA'，只要读写一致即可
#define LORA_CFG_VER    1
#define LORA_CFG_ADDR      (0x00000000u)   // 你自己选一个 flash 区域地址
#define LORA_CFG_SECTOR_SZ (4096u)         // W25Q 4K 扇区


typedef struct __attribute__((packed))
{
    uint32_t magic;        // 'LORA'
    uint16_t version;      // 1
    uint16_t length;       // sizeof(LoraConfig)

    uint32_t freq;         // Hz, e.g. 915000000
    uint8_t  bandwidth;    // 0..3 -> {62.5k,125k,250k,500k} 对应 radio.c 的 Bandwidths[]
    uint8_t  spreading;    // 7..12
    uint8_t  coderate;     // 1..4 (4/5..4/8)
    int8_t   txPower;      // dBm (-3..22 常见)
    uint16_t preambleLen;  // symbols
    uint8_t  crcOn;        // 0/1
    uint8_t  iqInverted;   // 0/1
    uint8_t  publicNetwork;// 0=private,1=public
    uint8_t  reserved0;

    uint16_t crc16;        // CRC over bytes [0 .. crc16-1]
} LoraConfig;

void        LoraCfg_Init(void);
const LoraConfig* LoraCfg_Get(void);

void        LoraCfg_ResetDefault(void);
int  LoraCfg_Save(const LoraConfig *cfg);
int LoraCfg_Validate(const LoraConfig *cfg);
void LoraCfg_SetDefault(LoraConfig *cfg);
void LoraCfg_Load(LoraConfig *cfg);

/* 可选：命令解析（和 ntn_config 一样风格） */
int         LoraCfg_ProcessLine(const char *line);

#ifdef __cplusplus
}
#endif
