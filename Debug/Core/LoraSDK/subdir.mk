################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/LoraSDK/crc.c \
../Core/LoraSDK/radio.c \
../Core/LoraSDK/sx126x-board.c \
../Core/LoraSDK/sx126x.c 

OBJS += \
./Core/LoraSDK/crc.o \
./Core/LoraSDK/radio.o \
./Core/LoraSDK/sx126x-board.o \
./Core/LoraSDK/sx126x.o 

C_DEPS += \
./Core/LoraSDK/crc.d \
./Core/LoraSDK/radio.d \
./Core/LoraSDK/sx126x-board.d \
./Core/LoraSDK/sx126x.d 


# Each subdirectory must supply rules for building sources it contributes
Core/LoraSDK/%.o Core/LoraSDK/%.su Core/LoraSDK/%.cyclo: ../Core/LoraSDK/%.c Core/LoraSDK/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m0plus -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32L051xx -c -I../Core/Inc -I../Drivers/STM32L0xx_HAL_Driver/Inc -I../Drivers/STM32L0xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32L0xx/Include -I../Drivers/CMSIS/Include -I"D:/STNewProduct/productNTN/Modules/toNTN" -I"D:/STNewProduct/productNTN/Modules/RS485" -I"D:/STNewProduct/productNTN/Modules/Lora" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-Core-2f-LoraSDK

clean-Core-2f-LoraSDK:
	-$(RM) ./Core/LoraSDK/crc.cyclo ./Core/LoraSDK/crc.d ./Core/LoraSDK/crc.o ./Core/LoraSDK/crc.su ./Core/LoraSDK/radio.cyclo ./Core/LoraSDK/radio.d ./Core/LoraSDK/radio.o ./Core/LoraSDK/radio.su ./Core/LoraSDK/sx126x-board.cyclo ./Core/LoraSDK/sx126x-board.d ./Core/LoraSDK/sx126x-board.o ./Core/LoraSDK/sx126x-board.su ./Core/LoraSDK/sx126x.cyclo ./Core/LoraSDK/sx126x.d ./Core/LoraSDK/sx126x.o ./Core/LoraSDK/sx126x.su

.PHONY: clean-Core-2f-LoraSDK

