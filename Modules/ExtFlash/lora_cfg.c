

#include "lora_cfg.h"   // 这里面有 typedef LoraConfig
#include "extflash_w25q16.h"
#include "crc16_ccitt.h"



void LoraCfg_SetDefault(LoraConfig *cfg)
{
    cfg->magic = LORA_CFG_MAGIC;
    cfg->version = LORA_CFG_VER;

    cfg->freq = 915000000;
    cfg->bandwidth = 1;        // 125k
    cfg->spreading = 7;
    cfg->coderate = 1;         // 4/5
    cfg->txPower = 14;

    cfg->preambleLen = 8;
    cfg->crcOn = 1;
    cfg->iqInverted = 0;
    cfg->publicNetwork = 0;
    cfg->length = sizeof(LoraConfig);
}


extern ExtFlash g_flash;   // 你的全局 flash 句柄（在别处 init 过）

void LoraCfg_Load(LoraConfig *cfg)
{
    // 读出整块配置
    int ok = ExtFlash_Read(&g_flash, LORA_CFG_ADDR, (uint8_t*)cfg, sizeof(LoraConfig));

    if (!ok || !LoraCfg_Validate(cfg))
    {
        LoraCfg_SetDefault(cfg);
        LoraCfg_Save(cfg);
    }
}
int LoraCfg_Validate(const LoraConfig *cfg)
{
    if (cfg->magic   != LORA_CFG_MAGIC) return 0;
    if (cfg->version != LORA_CFG_VER)   return 0;
    if (cfg->length  != sizeof(LoraConfig)) return 0;

    uint16_t crc = crc16_ccitt(
        (const uint8_t*)cfg,
        offsetof(LoraConfig, crc16),
        0xFFFF
    );

    return (crc == cfg->crc16);
}

int LoraCfg_Save(const LoraConfig *cfg)
{
	  LoraConfig tmp = *cfg;
	    tmp.magic   = LORA_CFG_MAGIC;
	    tmp.version = LORA_CFG_VER;
	    tmp.length  = sizeof(LoraConfig);

	    tmp.crc16 = crc16_ccitt(
	        (const uint8_t*)&tmp,
	        offsetof(LoraConfig, crc16),
	        0xFFFF
	    );

    uint32_t sector = LORA_CFG_ADDR & ~(4096u - 1);

    if (!ExtFlash_Erase4K(&g_flash, sector, 5000))
        return 0;

    if (!ExtFlash_Write(&g_flash,
                        LORA_CFG_ADDR,
                        (const uint8_t*)&tmp,
                        sizeof(tmp),
                        5000))
        return 0;

    return 1;
}

const LoraConfig* LoraCfg_Get(void)
{
	   static LoraConfig cfg;
	    static int inited = 0;

	    if (!inited)
	    {
	        LoraCfg_Load(&cfg);   // ✅ 从Flash读，不通过就默认+保存
	        inited = 1;
	    }
	    return &cfg;
}


