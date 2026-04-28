################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Modules/ExtFlash/crc16_ccitt.c \
../Modules/ExtFlash/extflash_w25q16.c \
../Modules/ExtFlash/lora_cfg.c \
../Modules/ExtFlash/ntn_config.c 

OBJS += \
./Modules/ExtFlash/crc16_ccitt.o \
./Modules/ExtFlash/extflash_w25q16.o \
./Modules/ExtFlash/lora_cfg.o \
./Modules/ExtFlash/ntn_config.o 

C_DEPS += \
./Modules/ExtFlash/crc16_ccitt.d \
./Modules/ExtFlash/extflash_w25q16.d \
./Modules/ExtFlash/lora_cfg.d \
./Modules/ExtFlash/ntn_config.d 


# Each subdirectory must supply rules for building sources it contributes
Modules/ExtFlash/%.o Modules/ExtFlash/%.su Modules/ExtFlash/%.cyclo: ../Modules/ExtFlash/%.c Modules/ExtFlash/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m0plus -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32L051xx -c -I../Core/Inc -I../Drivers/STM32L0xx_HAL_Driver/Inc -I../Drivers/STM32L0xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32L0xx/Include -I../Drivers/CMSIS/Include -I"D:/STNewProduct/productNTN/Modules/toNTN" -I"D:/STNewProduct/productNTN/Core/LoraSDK" -I"D:/STNewProduct/productNTN/Modules/ExtFlash" -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-Modules-2f-ExtFlash

clean-Modules-2f-ExtFlash:
	-$(RM) ./Modules/ExtFlash/crc16_ccitt.cyclo ./Modules/ExtFlash/crc16_ccitt.d ./Modules/ExtFlash/crc16_ccitt.o ./Modules/ExtFlash/crc16_ccitt.su ./Modules/ExtFlash/extflash_w25q16.cyclo ./Modules/ExtFlash/extflash_w25q16.d ./Modules/ExtFlash/extflash_w25q16.o ./Modules/ExtFlash/extflash_w25q16.su ./Modules/ExtFlash/lora_cfg.cyclo ./Modules/ExtFlash/lora_cfg.d ./Modules/ExtFlash/lora_cfg.o ./Modules/ExtFlash/lora_cfg.su ./Modules/ExtFlash/ntn_config.cyclo ./Modules/ExtFlash/ntn_config.d ./Modules/ExtFlash/ntn_config.o ./Modules/ExtFlash/ntn_config.su

.PHONY: clean-Modules-2f-ExtFlash

