################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Modules/Lora/lorarx.c 

OBJS += \
./Modules/Lora/lorarx.o 

C_DEPS += \
./Modules/Lora/lorarx.d 


# Each subdirectory must supply rules for building sources it contributes
Modules/Lora/%.o Modules/Lora/%.su Modules/Lora/%.cyclo: ../Modules/Lora/%.c Modules/Lora/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m0plus -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32L051xx -c -I../Core/Inc -I../Drivers/STM32L0xx_HAL_Driver/Inc -I../Drivers/STM32L0xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32L0xx/Include -I../Drivers/CMSIS/Include -I"D:/STNewProduct/productNTN/Modules/toNTN" -I"D:/STNewProduct/productNTN/Core/LoraSDK" -I"D:/STNewProduct/productNTN/Modules/ExtFlash" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-Modules-2f-Lora

clean-Modules-2f-Lora:
	-$(RM) ./Modules/Lora/lorarx.cyclo ./Modules/Lora/lorarx.d ./Modules/Lora/lorarx.o ./Modules/Lora/lorarx.su

.PHONY: clean-Modules-2f-Lora

