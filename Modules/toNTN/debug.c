#include "usart.h"
#include <string.h>

// 声明在 usart.c 里生成的句柄
extern UART_HandleTypeDef hlpuart1;

void Simple_Print(const char *s)
{
    if (!s) return;
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)s, strlen(s), HAL_MAX_DELAY);
}
int __io_putchar(int ch)
{
	 uint8_t c = (uint8_t)ch;
	    if (HAL_UART_Transmit(&hlpuart1, &c, 1, 5) != HAL_OK)
	    {
	        // 超时或错误：丢掉该字符，绝对不要卡死
	    }
	    return ch;
}
