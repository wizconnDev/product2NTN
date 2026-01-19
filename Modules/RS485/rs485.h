#ifndef RS485_UART_H
#define RS485_UART_H
#include "stm32l0xx_hal.h" // 需要 UART_HandleTypeDef 定义

#define WIND_RX_BUF_SIZE   64
#define RS485_RX_TOTAL_MS  200   // 固定总超时（150~300ms都可）

// RS485 走 USART2
extern UART_HandleTypeDef huart2;
extern uint8_t rs485_rx_byte;
extern uint8_t wind_active;
void RS485_UART_Init(void);
void RS485_UART_RxHandler(void);
void RS485U2_CheckAndSendToNTN(void);
void RS485_WindQuery_Start(void);
void RS485_WindQuery_Poll(void);


void RS485_TestSendToNTN_Poll(void);

#endif
