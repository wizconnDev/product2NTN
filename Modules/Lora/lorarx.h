#ifndef __LORARX_H
#define __LORARX_H

#include <stdint.h>

void LoraRx_Init(void);
void LoraRx_Process(void);
// 新增：EXTI 回调里调用，置位即可
void LoraRx_IrqNotify(void);
#endif
