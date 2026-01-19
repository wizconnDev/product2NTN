#include "sx126x-board.h"
#include "sx126x.h"
#include "radio.h"
#include "main.h"

extern SPI_HandleTypeDef hspi1;

/* ===== 你的硬件映射（按你的原理图/IOC） ===== */
#define LORA_NSS_PORT   GPIOA
#define LORA_NSS_PIN    GPIO_PIN_4

#define LORA_RST_PORT   GPIOB
#define LORA_RST_PIN    GPIO_PIN_0

#define LORA_BUSY_PORT  GPIOA
#define LORA_BUSY_PIN   GPIO_PIN_9

static inline void NSS_L(void){ HAL_GPIO_WritePin(LORA_NSS_PORT, LORA_NSS_PIN, GPIO_PIN_RESET); }
static inline void NSS_H(void){ HAL_GPIO_WritePin(LORA_NSS_PORT, LORA_NSS_PIN, GPIO_PIN_SET);   }

void HAL_Delay_nMS(uint32_t time_ms)
{
    HAL_Delay(time_ms);
}

static uint8_t SpiReadWrite(uint8_t data)
{
    uint8_t rx = 0;
    HAL_SPI_TransmitReceive(&hspi1, &data, &rx, 1, 1000);
    return rx;
}

static void SpiWrite(uint8_t *buf, uint16_t len)
{
    HAL_SPI_Transmit(&hspi1, buf, len, 1000);
}

static void SpiRead(uint8_t *buf, uint16_t len)
{
    uint8_t dummy = 0x00;
    for(uint16_t i=0;i<len;i++)
    {
        buf[i] = SpiReadWrite(dummy);
    }
}

void SX126xReset(void)
{
    HAL_Delay(10);
    HAL_GPIO_WritePin(LORA_RST_PORT, LORA_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(20);
    HAL_GPIO_WritePin(LORA_RST_PORT, LORA_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(10);
}

void SX126xWaitOnBusy(void)
{
    uint32_t t0 = HAL_GetTick();
    while(HAL_GPIO_ReadPin(LORA_BUSY_PORT, LORA_BUSY_PIN) == GPIO_PIN_SET)
    {
        if(HAL_GetTick() - t0 > 300) break; // 防死等
    }
}

void SX126xWakeup(void)
{
    NSS_L();
    SpiReadWrite(RADIO_GET_STATUS);
    SpiReadWrite(0x00);
    NSS_H();
    SX126xWaitOnBusy();
}

void SX126xWriteCommand(RadioCommands_t opcode, uint8_t *buffer, uint16_t size)
{
    SX126xCheckDeviceReady();
    NSS_L();
    SpiReadWrite((uint8_t)opcode);
    if(size) SpiWrite(buffer, size);
    NSS_H();

    if(opcode != RADIO_SET_SLEEP)
    {
        SX126xWaitOnBusy();
    }
}

void SX126xReadCommand(RadioCommands_t opcode, uint8_t *buffer, uint16_t size)
{
    SX126xCheckDeviceReady();
    NSS_L();
    SpiReadWrite((uint8_t)opcode);
    SpiReadWrite(0x00);        // dummy
    if(size) SpiRead(buffer, size);
    NSS_H();
    SX126xWaitOnBusy();
}

void SX126xWriteRegisters(uint16_t address, uint8_t *buffer, uint16_t size)
{
    SX126xCheckDeviceReady();
    NSS_L();
    SpiReadWrite(RADIO_WRITE_REGISTER);
    SpiReadWrite((address >> 8) & 0xFF);
    SpiReadWrite(address & 0xFF);
    SpiWrite(buffer, size);
    NSS_H();
    SX126xWaitOnBusy();
}

void SX126xWriteRegister(uint16_t address, uint8_t value)
{
    SX126xWriteRegisters(address, &value, 1);
}

void SX126xReadRegisters(uint16_t address, uint8_t *buffer, uint16_t size)
{
    SX126xCheckDeviceReady();
    NSS_L();
    SpiReadWrite(RADIO_READ_REGISTER);
    SpiReadWrite((address >> 8) & 0xFF);
    SpiReadWrite(address & 0xFF);
    SpiReadWrite(0x00);        // dummy
    if(size) SpiRead(buffer, size);
    NSS_H();
    SX126xWaitOnBusy();
}

uint8_t SX126xReadRegister(uint16_t address)
{
    uint8_t v = 0;
    SX126xReadRegisters(address, &v, 1);
    return v;
}

void SX126xWriteBuffer(uint8_t offset, uint8_t *buffer, uint8_t size)
{
    SX126xCheckDeviceReady();
    NSS_L();
    SpiReadWrite(RADIO_WRITE_BUFFER);
    SpiReadWrite(offset);
    SpiWrite(buffer, size);
    NSS_H();
    SX126xWaitOnBusy();
}

void SX126xReadBuffer(uint8_t offset, uint8_t *buffer, uint8_t size)
{
    SX126xCheckDeviceReady();
    NSS_L();
    SpiReadWrite(RADIO_READ_BUFFER);
    SpiReadWrite(offset);
    SpiReadWrite(0x00); // dummy
    SpiRead(buffer, size);
    NSS_H();
    SX126xWaitOnBusy();
}

void SX126xSetRfTxPower(int8_t power)
{
    SX126xSetTxParams(power, RADIO_RAMP_200_US);
}

uint8_t SX126xGetPaSelect(uint32_t channel)
{
    /* LLCC68 与 SX1262 兼容路径，先固定返回 SX1262 */
    return SX1262;
}

void SX126xAntSwOn(void)  { }
void SX126xAntSwOff(void) { }

bool SX126xCheckRfFrequency(uint32_t frequency)
{
    return true;
}
