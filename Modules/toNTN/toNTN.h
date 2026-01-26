#ifndef __TO_NTN_H__
#define __TO_NTN_H__

#include "main.h"
#include "stm32l0xx_hal.h"
typedef struct {
    char protocol[8];
    char ip[32];
    int port;
    char guid[16];   // 设备唯一 ID
} NetConfig;




#define NTN_RX_BUF_SZ 1536  // 统一缓冲区大小

extern uint8_t ntn_rx_buf[NTN_RX_BUF_SZ];
extern volatile int ntn_rx_len;
extern UART_HandleTypeDef huart1;     // NTN UART
extern UART_HandleTypeDef hlpuart1;   // PC UART

extern volatile int g_ntn_cfg_ready;
extern NetConfig g_ntn_cfg;

void NTN_SendTest(void);
void NTN_SendHttpGet(const char *guid, const char *host, int socket_id);
int NTN_SendPayloadUsingCfg(const uint8_t *payload, int len);
int NTN_EnsureConfigReady(int max_retry, uint32_t wait_ms_each);
int NTN_SendHelloUsingCfg(void);
void NTN_SendTest(void);   // 如果 main.c 要直接调的话
int NTN_Send_Payload_WithSrc(uint8_t src, const uint8_t *data, uint16_t len);
extern volatile int g_ntn_hello_done ;
void NTN_Invalidate_UserSock_AndCfg(const char* reason);
void NTN_FlushLoRaQueue(void);
int NTN_EnqueueLoRa(const uint8_t *data, uint16_t len);
#endif
