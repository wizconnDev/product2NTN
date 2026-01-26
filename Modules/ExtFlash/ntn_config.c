#include "ntn_config.h"
#include "extflash_w25q16.h"
#include "crc16_ccitt.h"
#include "string.h"
#include "stdio.h"
#include <ctype.h>
#include "lora_cfg.h"

#include <stddef.h>
#include <stdlib.h>
/* ====== 你需要在这里对上你的硬件资源 ======
   - SPI2 句柄：例如 extern SPI_HandleTypeDef hspi2;
   - CS 引脚：例如 FLASH_CS_GPIO_Port / FLASH_CS_Pin
*/
#include "spi.h"
#include "gpio.h"

extern SPI_HandleTypeDef hspi2;

/* 这里按你的网名改：SPI2_CS */
#ifndef FLASH_CS_GPIO_Port
#define FLASH_CS_GPIO_Port   GPIOB
#endif
#ifndef FLASH_CS_Pin
#define FLASH_CS_Pin         GPIO_PIN_12
#endif

/* 分区地址（4KB 对齐） */
#define NTN_CFG_ADDR   (0x001000u)
#define NTN_CFG_MAGIC  (0x434E544Eu) /* 'NTNC' little-endian: 0x4E544E43 -> 我们用读写一致即可 */

ExtFlash  g_flash;
static NtnConfig g_cfg;
static LoraConfig g_lora;
static void default_cfg(NtnConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->magic = NTN_CFG_MAGIC;
    c->version = 1;
    c->length  = (uint16_t)sizeof(NtnConfig);

    c->rs485_baud = 9600;
    c->ntn_baud   = 9600;

    strncpy(c->host, "47.103.38.50", sizeof(c->host)-1);
    c->port  = 80;
    c->proto = 0; // TCP
}

static uint16_t calc_crc(const NtnConfig *c)
{
    /* crc 不包含最后 crc16 字段本身 */
    return crc16_ccitt((const uint8_t*)c, offsetof(NtnConfig, crc16), 0xFFFF);
}

static int cfg_valid(const NtnConfig *c)
{
    if (c->magic != NTN_CFG_MAGIC) return 0;
    if (c->version != 1) return 0;
    if (c->length != sizeof(NtnConfig)) return 0;

    uint16_t crc = calc_crc(c);
    return (crc == c->crc16) ? 1 : 0;
}

/* 你可以把“应用波特率 / 触发重连”等动作放在这里
   注意：不要在 UART 正在接收中断疯狂跑的时候直接改波特率。
   更稳的是 SAVE 后提示重启，或你做一个“延迟应用”标志。 */
__attribute__((weak)) void NTN_Config_OnSaved(const NtnConfig *cfg)
{
    (void)cfg;
}

void NTN_Config_Init(void)
{
    ExtFlash_Init(&g_flash, &hspi2, FLASH_CS_GPIO_Port, FLASH_CS_Pin);

    /* 可选：读 JEDEC 让你在 log 里确认 flash 在 */
    /* uint8_t id[3]; ExtFlash_ReadJedecId(&g_flash, id); */

    /* 读配置 */
    NtnConfig tmp;
    if (!ExtFlash_Read(&g_flash, NTN_CFG_ADDR, (uint8_t*)&tmp, sizeof(tmp)))
    {
        default_cfg(&g_cfg);
        g_cfg.crc16 = calc_crc(&g_cfg);
        LoraCfg_Load(&g_lora);
        return;
    }

    if (!cfg_valid(&tmp))
    {
        default_cfg(&g_cfg);
        g_cfg.crc16 = calc_crc(&g_cfg);
        LoraCfg_Load(&g_lora);
        return;
    }

    g_cfg = tmp;
    LoraCfg_Load(&g_lora);
}

const NtnConfig* NTN_Config_Get(void)
{
    return &g_cfg;
}

void NTN_Config_ResetDefault(void)
{
    default_cfg(&g_cfg);
    g_cfg.crc16 = calc_crc(&g_cfg);
}

int NTN_Config_Save(void)
{
    /* 生成最终要写入的 blob：确保 padding/未用字节一致 */
    NtnConfig out;
    memset(&out, 0, sizeof(out));
    out = g_cfg;

    out.magic   = NTN_CFG_MAGIC;
    out.version = 1;
    out.length  = (uint16_t)sizeof(NtnConfig);
    out.crc16   = calc_crc(&out);

    /* 擦 4KB sector */
    if (!ExtFlash_Erase4K(&g_flash, NTN_CFG_ADDR, 5000)) return 0;

    /* 写入 */
    if (!ExtFlash_Write(&g_flash, NTN_CFG_ADDR, (const uint8_t*)&out, sizeof(out), 2000)) return 0;

    /* verify */
    NtnConfig ver;
    if (!ExtFlash_Read(&g_flash, NTN_CFG_ADDR, (uint8_t*)&ver, sizeof(ver))) return 0;
    if (memcmp(&out, &ver, sizeof(out)) != 0) return 0;

    g_cfg = out;
    NTN_Config_OnSaved(&g_cfg);
    return 1;


}

/* ===== 命令解析（简洁版，够你现在用） ===== */

static int strieq(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = (char)toupper((unsigned char)*a++);
        char cb = (char)toupper((unsigned char)*b++);
        if (ca != cb) return 0;
    }
    return (*a == 0 && *b == 0);
}

static void trim_space(char *s)
{
    /* left */
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);

    /* right */
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n-1])) s[--n] = 0;
}

static const char* skip_word(const char *s)
{
    while (*s && !isspace((unsigned char)*s)) s++;
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* ===== LoRa 参数显示转换（仅用于 READ 打印） ===== */

static unsigned bw_to_khz(uint8_t bw_enum)
{
    // SDK: 0=125k, 1=250k, 2=500k
    switch (bw_enum) {
        case 0: return 125;
        case 1: return 250;
        case 2: return 500;
        default: return 0; // unknown
    }
}

static const char* cr_to_str(uint8_t cr_enum)
{
    // 常见 LoRa SDK: 0=4/5, 1=4/6, 2=4/7, 3=4/8
    // 你现在用的是 coderate=1 代表 4/6 或 4/5？取决于 SDK。
    // 如果你确认 radio.h 的枚举，请按它调整。
    switch (cr_enum) {
        case 0: return "4/5";
        case 1: return "4/6";
        case 2: return "4/7";
        case 3: return "4/8";
        default: return "?";
    }
}

static const char* onoff(uint8_t v)
{
    return v ? "ON" : "OFF";
}

static const char* pubnet_str(uint8_t v)
{
    // 你 SDK 的 public/private syncword
    return v ? "PUBLIC" : "PRIVATE";
}


int NTN_Config_ProcessLine(const char *line_in)
{
	Simple_Print("[CFG] enter NTN_Config_ProcessLine\r\n");
    if (!line_in) return 0;

    char line[160];
    strncpy(line, line_in, sizeof(line)-1);
    line[sizeof(line)-1] = 0;
    trim_space(line);

    /* 支持：AT+NTNCFG ... 或 NTNCFG ... */
    const char *p = line;
    if (!strncasecmp(p, "AT+NTNCFG", 9)) p = skip_word(p);
    else if (!strncasecmp(p, "NTNCFG", 5)) p = skip_word(p);
    else return 0; // not ours

    if (!*p) return 0;

    /* READ */
    if (!strncasecmp(p, "READ", 4))
    {
        /* 这里不直接 printf，避免你工程没有stdio；
           你可以改成 Simple_Print 或写一个 callback */
    	   char buf[320];
    	   snprintf(buf, sizeof(buf),
    	     "NTNCFG: HOST=%s PORT=%u PROTO=%u RS485_BAUD=%lu NTN_BAUD=%lu "
    	     "LORA_FREQ=%luHz LORA_BW=%ukHz LORA_SF=SF%u LORA_CR=%s LORA_TXP=%ddBm "
    	     "LORA_CRC=%s LORA_IQ=%s LORA_NET=%s LORA_PRE=%u\r\n",
    	     g_cfg.host,
    	     (unsigned)g_cfg.port,
    	     (unsigned)g_cfg.proto,
    	     (unsigned long)g_cfg.rs485_baud,
    	     (unsigned long)g_cfg.ntn_baud,

    	     (unsigned long)g_lora.freq,
    	     bw_to_khz(g_lora.bandwidth),
    	     (unsigned)g_lora.spreading,
    	     cr_to_str(g_lora.coderate),
    	     (int)g_lora.txPower,

    	     onoff(g_lora.crcOn),
    	     g_lora.iqInverted ? "INVERT" : "NORMAL",
    	     pubnet_str(g_lora.publicNetwork),
    	     (unsigned)g_lora.preambleLen
    	   );



    	    Simple_Print(buf);   // 或你自己的输出函数
    	    return 1;
    }

    /* RESET */
    if (!strncasecmp(p, "RESET", 5))
    {
        NTN_Config_ResetDefault();
        return 1;
    }

    /* SAVE */
      if (!strncasecmp(p, "SAVE", 4))
      {
          int ok1 = NTN_Config_Save();
          int ok2 = LoraCfg_Save(&g_lora);

          if (ok1 && ok2)
          {
              /* ★让 LoRa “立刻生效”
                 这里不要新增新函数名：你项目里如果已经有 Radio_ApplyLoraConfig，就用它；
                 没有的话，你把这一行替换成你现有的“重新配置 LoRa + 重新进 continuous RX”的函数。 */
              Radio_ApplyLoraConfig(&g_lora);   // <-- 如果你工程里这个函数名不同，改成你自己的即可
              return 1;
          }
          return -1;
      }


    /* SET ... key=value key=value */
    if (!strncasecmp(p, "SET", 3))
    {
        p = skip_word(p);

        /* 简单 token：用空格分隔 key=value */
        while (*p)
        {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;

            char kv[80];
            size_t i = 0;
            while (*p && !isspace((unsigned char)*p) && i < sizeof(kv)-1) kv[i++] = *p++;
            kv[i] = 0;

            char *eq = strchr(kv, '=');
            if (!eq) continue;
            *eq++ = 0;

            /* upper key */
            for (char *k = kv; *k; k++) *k = (char)toupper((unsigned char)*k);

            if (strieq(kv, "HOST"))
            {
                strncpy(g_cfg.host, eq, sizeof(g_cfg.host)-1);
                g_cfg.host[sizeof(g_cfg.host)-1] = 0;
            }
            else if (strieq(kv, "PORT"))
            {
                g_cfg.port = (uint16_t)atoi(eq);
            }
            else if (strieq(kv, "PROTO"))
            {
                if (!strncasecmp(eq, "TCP", 3)) g_cfg.proto = 0;
                else if (!strncasecmp(eq, "UDP", 3)) g_cfg.proto = 1;
            }
            else if (strieq(kv, "RS485_BAUD"))
            {
                g_cfg.rs485_baud = (uint32_t)atoi(eq);
            }
            else if (strieq(kv, "NTN_BAUD"))
            {
                g_cfg.ntn_baud = (uint32_t)atoi(eq);
            }

            else if (strieq(kv, "LORA_FREQ"))
            {
                g_lora.freq = (uint32_t)strtoul(eq, NULL, 10);
            }
            else if (strieq(kv, "LORA_BW"))
            {
                /* 重要：按你 LoRaSDK 的约定：0=125k,1=250k,2=500k
                   你可以输入 0/1/2，也可以输入 125/250/500（下面都支持） */
                int bw = atoi(eq);
                if (bw == 0 || bw == 1 || bw == 2)
                    g_lora.bandwidth = (uint8_t)bw;
                else if (bw == 125)
                    g_lora.bandwidth = 0;
                else if (bw == 250)
                    g_lora.bandwidth = 1;
                else if (bw == 500)
                    g_lora.bandwidth = 2;
            }
            else if (strieq(kv, "LORA_SF"))
            {
                g_lora.spreading = (uint8_t)atoi(eq);
            }
            else if (strieq(kv, "LORA_CR"))
            {
                g_lora.coderate = (uint8_t)atoi(eq);
            }
            else if (strieq(kv, "LORA_TXP"))
            {
                g_lora.txPower = (int8_t)atoi(eq);
            }
            else if (strieq(kv, "LORA_CRC"))
            {
                g_lora.crcOn = (uint8_t)atoi(eq);
            }
            else if (strieq(kv, "LORA_IQ"))
            {
                g_lora.iqInverted = (uint8_t)atoi(eq);
            }
            else if (strieq(kv, "LORA_PUB"))
            {
                g_lora.publicNetwork = (uint8_t)atoi(eq);
            }
            else if (strieq(kv, "LORA_PRE"))
            {
                g_lora.preambleLen = (uint16_t)atoi(eq);
            }

        }

        /* SET 只是改 RAM，不落盘 */
        g_cfg.crc16 = calc_crc(&g_cfg);
        return 1;
    }



    return 0;
}
