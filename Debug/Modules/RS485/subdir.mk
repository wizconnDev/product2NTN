################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Modules/RS485/rs485.c 

OBJS += \
./Modules/RS485/rs485.o 

C_DEPS += \
./Modules/RS485/rs485.d 


# Each subdirectory must supply rules for building sources it contributes
Modules/RS485/%.o Modules/RS485/%.su Modules/RS485/%.cyclo: ../Modules/RS485/%.c Modules/RS485/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m0plus -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32L051xx -c -I../Core/Inc -I../Drivers/STM32L0xx_HAL_Driver/Inc -I../Drivers/STM32L0xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32L0xx/Include -I../Drivers/CMSIS/Include -I"D:/STNewProduct/productNTN/Modules/toNTN" -I"D:/STNewProduct/productNTN/Core/LoraSDK" -I"D:/STNewProduct/productNTN/Modules/ExtFlash" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-Modules-2f-RS485

clean-Modules-2f-RS485:
	-$(RM) ./Modules/RS485/rs485.cyclo ./Modules/RS485/rs485.d ./Modules/RS485/rs485.o ./Modules/RS485/rs485.su

.PHONY: clean-Modules-2f-RS485

