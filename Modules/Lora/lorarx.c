#include "lorarx.h"
#include "radio.h"
#include "sx126x.h"
#include <string.h>
#include <stdio.h>
#include "toNTN.h"

static RadioEvents_t RadioEvents;

static uint8_t rxBuf[256];

static volatile uint8_t g_lora_irq_pending = 0;

void LoraRx_IrqNotify(void)
{
    g_lora_irq_pending = 1;
}

static void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr)
{
    // 1) 拷贝
    if (size > sizeof(rxBuf)) size = sizeof(rxBuf);
    memcpy(rxBuf, payload, size);

    // 2) 入队
    int ok = NTN_EnqueueLoRa(rxBuf, size);

    // 3) 每秒最多打印一次（避免数据洪导致系统卡死）
    static uint32_t last_log = 0;
    uint32_t now = HAL_GetTick();
    if ((uint32_t)(now - last_log) >= 1000)
    {
        last_log = now;
        printf("[LORA] RX len=%u rssi=%d snr=%d enqueue=%d\r\n",
               (unsigned)size, (int)rssi, (int)snr, ok);
    }

    // 4) 继续 RX
    Radio.Rx(0);
}


static void OnRxTimeout(void)
{
    printf("[LORA][RX_TIMEOUT]\r\n");
    Radio.Rx(0);
}

static void OnRxError(void)
{
    // CRC error / header error 等会走这里（取决于库）
    printf("[LORA][RX_ERROR]\r\n");
    Radio.Rx(0);
}

void LoraRx_Init(void)
{
    memset(&RadioEvents, 0, sizeof(RadioEvents));
    RadioEvents.RxDone = OnRxDone;
    RadioEvents.RxTimeout = OnRxTimeout;
    RadioEvents.RxError = OnRxError;

    Radio.Init(&RadioEvents);

    // 你的模块 DIO2 内部接RF开关 → 必须打开
    SX126xSetDio2AsRfSwitchCtrl(true);

    // 必须和 TX 一模一样
    Radio.SetChannel(915000000);

    // RX 参数必须匹配 TX：BW125 / SF7 / CR4/5 / CRC ON / 显式头
    // 下面参数是 Semtech 常见格式（如果你的库枚举不同，我们再按编译通过的来调）
    Radio.SetRxConfig(MODEM_LORA,
                      1,      // BW125k（有的库是0，有的库是1；若收不到再对照你的枚举）
                      7,      // SF7
                      1,      // CR4/5
                      0,      // AFC (FSK用，LoRa填0)
                      8,      // preamble length
                      0,      // symb timeout（0=库默认）
                      false,  // fixLen = false（显式长度）
                      0,      // payloadLen（fixLen才用）
                      true,   // CRC ON
                      0,      // freqHopOn
                      0,      // hopPeriod
                      false,  // IQ inversion（与TX一致，TX默认false）
                      true);  // continuous

    printf("[LORA] RX init OK, enter continuous RX\r\n");
    Radio.Rx(0); // continuous RX
}

void LoraRx_Process(void)
{
	if (g_lora_irq_pending)
	    {
	        g_lora_irq_pending = 0;

	        // 关键：触发 radio.c 里 IrqFired=true
	        RadioOnDioIrq();

	        // 真正解析 IRQ/回调 RxDone
	        Radio.IrqProcess();
	    }
}
