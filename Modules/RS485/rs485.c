#include "rs485.h"
#include "toNTN.h"
#include "usart.h"      // 为了用 hlpuart1 打调试信息
#include <string.h>
#include <stdint.h>
// ================== 旧 RS485 逻辑，先“冷冻”起来 ==================
#if 0
// ===== ultra-min 三段式：RS485 weather station Uart =====
#define RS485U2_BUF_SIZE 128
#define RS485U2_TIMEOUT_MS 2
static char rs485U2_buf[RS485U2_BUF_SIZE];
static uint16_t rs485U2_index = 0;


void RS485_UART2_Init(void) {
    // 首次启动中断
    HAL_UART_Receive_IT(&huart2, &rs485U2_rx_byte, 1);
}

void RS485_UART2_RxHandler(void) {
	   last_rx_tick = HAL_GetTick();
	    receiving    = 1;
	    if (rs485U2_index < RS485U2_BUF_SIZE - 1) {
	    	rs485U2_buf[rs485U2_index++] = (char)rs485U2_rx_byte;
	    }
	    HAL_UART_Receive_IT(&huart2, &rs485U2_rx_byte, 1);

}
// 计算“每字符时间（毫秒）”，向上取整
static uint32_t char_time_ms(uint32_t baud) {
    // 10 比特 × 1000 毫秒 / baud
    // + (baud-1) 保证向上取整
    return (10 * 1000 + baud - 1) / baud;
}

// 计算帧超时：取 4 倍字符时间
static uint32_t frame_timeout_ms(uint32_t baud) {
    uint32_t ct = char_time_ms(baud);
    return ct * 4;
}

// 定时或主循环里定期调用：超时判定为一条消息结束
void RS485U2_CheckTimeout(void)
{
	uint32_t timeout = frame_timeout_ms(huart2.Init.BaudRate);
    if (!receiving) return;
    if (HAL_GetTick() - last_rx_tick < timeout) return;

    // 超时：认为一条命令到齐
    receiving = 0;
    rs485U2_buf[rs485U2_index] = '\0';

    // 去掉尾部所有空格／Tab／CR／LF
    int len = rs485U2_index;
    while (len > 0 && (rs485U2_buf[len-1]==' ' || rs485U2_buf[len-1]=='\t'
                     || rs485U2_buf[len-1]=='\r' || rs485U2_buf[len-1]=='\n')) {
        rs485U2_buf[--len] = '\0';
    }

    // 只有长度 >= 3 的才算有效
    if (len >= 3) {
        message_ready = true;
    } else {
        Simple_Print(">>> RS485: msg too short, discard\r\n");
    }

    rs485U2_index = 0;
}

void RS485U2_CheckAndSendToNTN(void) {

	RS485U2_CheckTimeout();
	if (!message_ready) return;
    message_ready = false;

    // 跳过前导的空格／Tab
       char *cmd = rs485U2_buf;
    while (*cmd==' ' || *cmd=='\t') cmd++;

    // 调试：回显一行，确认到底收到什么
    Simple_Print("UART2 RECV: "); Simple_Print(cmd); Simple_Print("\r\n");
        // 如果以下面三种（不区分大小写）开头，就当配置命令
        if (   strncasecmp(cmd, "CFG READ", 8) == 0
            || strncasecmp(cmd, "CFG SET",  7) == 0
            || strncasecmp(cmd, "CFG SAVE", 8) == 0)
        {
            Simple_Print(">>> RS485: Config command\r\n");
            Config_ProcessCommand(cmd);
        }else {
        	NTN_Send_Payload_WithSrc(0x25,(const uint8_t *)cmd, strlen(cmd));
    	    Simple_Print(">>> RS485 to NTN forwarded\r\n");
    }
    // 4) 清空缓冲区，准备下次接收
       rx_index = 0;
       rs485U2_buf[0] = '\0';
}


//------------------------uart1 rs485 sensor code//


static const uint8_t CMD_WIND_DIRECTION[] = { 0x02,0x03,0x00,0x00,0x00,0x02,0xC4,0x38 };
static const uint8_t CMD_WIND_SPEED[] = { 0x01,0x03,0x00,0x00,0x00,0x01,0x84,0x0A };
static const uint8_t CMD_CO2[]        = { 0x02,0x03,0x00,0x00,0x00,0x02,0xC4,0x38 };
static const uint8_t CMD_HUMID[]      = { 0x01,0x03,0x00,0x00,0x00,0x01,0x84,0x0A };
static const uint8_t CMD_PRESS[]      = { 0x02,0x03,0x00,0x00,0x00,0x02,0xC4,0x38 };

static uint8_t  i = 0;

void RS485_UART_Init(void) {
    // 首次启动中断
    HAL_UART_Receive_IT(&huart1, &rs485_rx_byte, 1);
}

void RS485_WindQuery_Start(void)

{
	if (wind_active) return;
	wind_idx = 0;
    wind_active = 1;
    wind_deadline = HAL_GetTick() + RS485_RX_TOTAL_MS;

    if (i == 0)
    		{
    	   HAL_UART_Transmit(&huart1, (uint8_t*)CMD_WIND_DIRECTION, sizeof(CMD_WIND_DIRECTION), HAL_MAX_DELAY);
    		}else if (i == 1) {
    			HAL_UART_Transmit(&huart1, (uint8_t*)CMD_WIND_SPEED, sizeof(CMD_WIND_SPEED), HAL_MAX_DELAY);
    		}else if (i == 2) {
    			HAL_UART_Transmit(&huart1, (uint8_t*)CMD_CO2, sizeof(CMD_CO2), HAL_MAX_DELAY);
    		}else if (i == 3) {
    			HAL_UART_Transmit(&huart1, (uint8_t*)CMD_HUMID, sizeof(CMD_HUMID), HAL_MAX_DELAY);
    		}else if (i == 4) {
    			HAL_UART_Transmit(&huart1, (uint8_t*)CMD_PRESS, sizeof(CMD_PRESS), HAL_MAX_DELAY);
    		}

    i = (i + 1) % 5;

// 你要打印就留，不要就删：
   // Simple_Print("TX(485): 02 03 00 00 00 02 C4 38\r\n"); }
}

void RS485_UART_RxHandler(void)
{
	if (!wind_active) return;
    if (wind_idx < WIND_RX_BUF_SIZE)
    {
	wind_buf[wind_idx++] = (uint8_t)rs485_rx_byte;
    }
}

// 3) 在主循环里每次调一次：到了截止时间就原样上报
void RS485_WindQuery_Poll(void)
{
	if (!wind_active) return;
	if ((int32_t)(HAL_GetTick() - wind_deadline) < 0) return;
	wind_active = 0;
	uint16_t length = wind_idx;

	if (length > 0)
	{
		char line[3 * WIND_RX_BUF_SIZE + 4];
		int pos = 0;
		for (uint16_t k = 0; k < length && pos + 3 < sizeof(line); k++)
		{
			pos += snprintf(&line[pos], sizeof(line) - pos, "%02X ", wind_buf[k]);
		}
		Simple_Print("RX(485): ");
		Simple_Print(line);
		Simple_Print("\r\n");
		NTN_Send_Payload_WithSrc(0x25, wind_buf, length);
		// 直接发二进制 // 可要可不要的提示：
		// Simple_Print(">>> RS485 frame forwarded to NTN\r\n");
	}else
		{
			// 可要可不要的提示： // Simple_Print("RX(485): <timeout>\r\n"); } }
		}
}
#endif



/* ================== 485 传感器部分（走 USART2） ================== */

// 一帧 RS485 原始数据缓存
uint8_t rs485_rx_byte;                         // 单字节缓冲（给 HAL 用）
static uint8_t  wind_buf[WIND_RX_BUF_SIZE];    // 一帧数据
static uint16_t wind_idx       = 0;
uint8_t  wind_active    = 0;
static uint32_t wind_deadline  = 0;

// 可以先只用“风速”命令，后面再扩展成 i = 0..4 轮询
static const uint8_t CMD_WIND_SPEED[] = { 0x01,0x03,0x00,0x00,0x00,0x01,0x84,0x0A };

// 初始化：启动 USART2 中断接收
void RS485_UART_Init(void)
{
    HAL_UART_Receive_IT(&huart2, &rs485_rx_byte, 1);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)"RS485 UART2 IT ON\r\n", 19, 100);

}

// 发一条风速查询命令，启动接收窗口
void RS485_WindQuery_Start(void)
{
    if (wind_active) return;  // 上一帧还没结束就不再发

    wind_idx      = 0;
    wind_active   = 1;
    wind_deadline = HAL_GetTick() + RS485_RX_TOTAL_MS;

    // 直接通过 USART2 发命令
    HAL_UART_Transmit(&huart2,
                      (uint8_t*)CMD_WIND_SPEED,
                      sizeof(CMD_WIND_SPEED),
                      HAL_MAX_DELAY);
}

// 在 USART2 的 HAL_UART_RxCpltCallback 分支里调用

volatile uint32_t g_u2_rx_cnt = 0;
void RS485_UART_RxHandler(void)
{

	  g_u2_rx_cnt++;
    if (!wind_active) {
        // 即便没在“窗口期”，也不要乱写缓冲，直接丢弃
        return;
    }

    if (wind_idx < WIND_RX_BUF_SIZE)
    {
        wind_buf[wind_idx++] = rs485_rx_byte;
    }
    // 不在这里判断结束，只是收；结束用超时在 Poll 里判
}

// 轮询：到了超时时间，把一帧数据转发给 NTN
void RS485_WindQuery_Poll(void)
{
    if (!wind_active) return;

    // 还没到截止时间，继续等
    if ((int32_t)(HAL_GetTick() - wind_deadline) < 0)
        return;

    // 超时：认为一帧结束
    wind_active = 0;
    uint16_t length = wind_idx;
    wind_idx = 0;

    if (length == 0)
    {
        const char *msg = "RS485: <timeout, no data>\r\n";
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)msg, strlen(msg), 100);
        return;
    }

    // 打印 HEX 看一眼收到什么
    char line[3 * WIND_RX_BUF_SIZE + 4];
    int pos = 0;
    for (uint16_t k = 0; k < length && pos + 3 < sizeof(line); k++)
    {
        pos += snprintf(&line[pos], sizeof(line) - pos, "%02X ", wind_buf[k]);
    }
    const char *prefix = "RX(485): ";
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)prefix, strlen(prefix), 100);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)line, strlen(line), 100);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)"\r\n", 2, 100);

    // 直接把这帧原始 RS485 数据丢给 NTN（后面可以再解析 Modbus 再拼 JSON）
    if (g_ntn_cfg_ready)
    {
        if (NTN_SendPayloadUsingCfg(wind_buf, length))
        {
            const char *msg = "RS485 frame forwarded via NTN OK\r\n";
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)msg, strlen(msg), 100);
        }
        else
        {
            const char *msg = "RS485 frame forwarded via NTN FAIL\r\n";
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)msg, strlen(msg), 100);
        }
    }
    else
    {
        const char *msg = "RS485 frame ready but cfg not ready\r\n";
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)msg, strlen(msg), 100);
    }
}



// ================== 简单测试：从 rs485.c 里发一条固定信息到 NTN ==================

void RS485_TestSendToNTN_Poll(void)
{
    static uint8_t  s_sent          = 0;      // 是否已经成功发过一次
    static uint8_t  s_cfg_seen      = 0;      // 是否已经见过 “cfg ready”
    static uint32_t s_cfg_ready_tick = 0;     // 第一次 cfg ready 的时刻

    // 只想发一次测试包
    if (s_sent)
        return;

    // 还没拿到 cfg，就啥也不干
    if (!g_ntn_cfg_ready)
        return;

    // 第一次看到 cfg ready：先记时间，不立刻发
    if (!s_cfg_seen) {
        s_cfg_seen       = 1;
        s_cfg_ready_tick = HAL_GetTick();
        return;
    }

    // cfg ready 后至少等 3000ms（你可以改成 2000/5000）
    if ((uint32_t)(HAL_GetTick() - s_cfg_ready_tick) < 3000)
        return;

    const uint8_t payload[] = "RS485 WIND TEST";

    if (NTN_SendPayloadUsingCfg(payload, (int)strlen((const char*)payload)))
    {
        const char *msg = "RS485 test payload sent via NTN OK\r\n";
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)msg, strlen(msg), 100);
        s_sent = 1;   // 成功一次就不再发
    }
    else
    {
        const char *msg = "RS485 test payload sent via NTN FAIL\r\n";
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)msg, strlen(msg), 100);
        // 这里失败就不置 s_sent，下次循环会继续尝试
    }
}














