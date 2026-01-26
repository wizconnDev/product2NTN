################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Modules/toNTN/debug.c \
../Modules/toNTN/toNTN.c 

OBJS += \
./Modules/toNTN/debug.o \
./Modules/toNTN/toNTN.o 

C_DEPS += \
./Modules/toNTN/debug.d \
./Modules/toNTN/toNTN.d 


# Each subdirectory must supply rules for building sources it contributes
Modules/toNTN/%.o Modules/toNTN/%.su Modules/toNTN/%.cyclo: ../Modules/toNTN/%.c Modules/toNTN/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m0plus -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32L051xx -c -I../Core/Inc -I../Drivers/STM32L0xx_HAL_Driver/Inc -I../Drivers/STM32L0xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32L0xx/Include -I../Drivers/CMSIS/Include -I"D:/STNewProduct/productNTN/Modules/toNTN" -I"D:/STNewProduct/productNTN/Core/LoraSDK" -I"D:/STNewProduct/productNTN/Modules/ExtFlash" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-Modules-2f-toNTN

clean-Modules-2f-toNTN:
	-$(RM) ./Modules/toNTN/debug.cyclo ./Modules/toNTN/debug.d ./Modules/toNTN/debug.o ./Modules/toNTN/debug.su ./Modules/toNTN/toNTN.cyclo ./Modules/toNTN/toNTN.d ./Modules/toNTN/toNTN.o ./Modules/toNTN/toNTN.su

.PHONY: clean-Modules-2f-toNTN

