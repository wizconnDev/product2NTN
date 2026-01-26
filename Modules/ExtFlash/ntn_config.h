#ifndef NTN_CONFIG_H
#define NTN_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 你可以按需加字段：APN、HOST、PORT、PROTO 等 */
typedef struct __attribute__((packed))
{
    uint32_t magic;      // 'NTNC'
    uint16_t version;    // 1
    uint16_t length;     // sizeof(NtnConfig)
    uint32_t rs485_baud; // 命令口波特率（风速传感器等外接传感器）
    uint32_t ntn_baud;   // NTN 模块口波特率（lpuart）
    char     host[48];   // "47.103.38.50" or domain
    uint16_t port;       // 80 / 443 ...
    uint8_t  proto;      // 0=TCP 1=UDP (你也可以扩展 HTTP)
    uint8_t  reserved0;
    uint16_t reserved1;
    uint16_t crc16;      // crc over bytes [0 .. crc16-1]
} NtnConfig;

void        NTN_Config_Init(void);
const NtnConfig* NTN_Config_Get(void);

int         NTN_Config_Save(void);
void        NTN_Config_ResetDefault(void);
void NTN_Config_OnSaved(const NtnConfig *cfg);


/* 你可以把 RS485 收到的一行命令丢进来：
   - "NTNCFG READ"
   - "NTNCFG SET HOST=... PORT=... PROTO=TCP"
   - "NTNCFG SET RS485_BAUD=115200"
   - "NTNCFG SAVE"
*/
int         NTN_Config_ProcessLine(const char *line);

#ifdef __cplusplus
}
#endif

#endif
