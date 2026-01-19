/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usart.h"
#include "spi.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "toNTN.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "rs485.h"
#include "lorarx.h"     // ⭐ 新增：声明 loraSPI_ReceiveLoop / Lora_SimulateRxTestOnce

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define NTN_STABLE_MS 15000u
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
uint8_t lpuart_rx;      // 来自 PC 的单字节
uint8_t ntn_rx;         // 来自 NTN 的单字节

// PC -> NTN 命令缓冲（按行缓存，直到 CRLF）
uint8_t  pc_cmd_buf[128];
volatile uint16_t pc_cmd_idx   = 0;
volatile uint8_t  pc_cmd_ready = 0;

extern NetConfig g_ntn_cfg;
extern volatile int g_ntn_cfg_ready;
extern volatile int g_ntn_hello_done;
extern const void* g_llcc68_ctx;
extern volatile uint8_t g_llcc68_last_status;
// POWERON 检测用：只记录时间戳，不在中断里清大缓冲
static volatile uint32_t g_last_poweron_tick = 0;

volatile uint8_t g_lora_irq = 0;

static char rx_str[128];


// ✅ POWERON 按行检测用（避免 strstr 全缓冲导致反复命中）
static char     ntn_line[128];
static uint16_t ntn_line_idx = 0;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_LPUART1_UART_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_SPI1_Init();
  MX_SPI2_Init();
  /* USER CODE BEGIN 2 */

 HAL_UART_Receive_IT(&hlpuart1, &lpuart_rx, 1); // PC
  HAL_UART_Receive_IT(&huart1,   &ntn_rx,  1); // NTN

  // 上电后先给模块一些时间（你原来有 10s，这里保留）
  g_last_poweron_tick = HAL_GetTick();
  HAL_Delay(10000);

  // RS485
  RS485_UART_Init();

  // LoRa
  printf("BOOT RX\r\n");

  LoraRx_Init();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */

  uint32_t next_try_tick = HAL_GetTick() + 10000;
  uint32_t last_tick = 0;

  // （可选）只发送一次 READY TEST
 static int ready_test_sent = 0;
  while (1)
  {

	  // 1) LoRa 永远处理（保证接收、回调、入队不断）
	      LoraRx_Process();

	      // A) PC -> NTN 透传（按行发送）
	      if (pc_cmd_ready)
	      {
	          __disable_irq();
	          uint16_t len = pc_cmd_idx;
	          pc_cmd_idx   = 0;
	          pc_cmd_ready = 0;
	          __enable_irq();

	          char b[64];
	          int n = snprintf(b, sizeof(b), "[PT] send to NTN len=%u\r\n", len);
	          HAL_UART_Transmit(&hlpuart1, (uint8_t*)b, n, 100);

	          if (len > 0)
	              HAL_UART_Transmit(&huart1, pc_cmd_buf, len, 1000);
	      }


	      // 2) 稳定窗口：只阻挡 NTN 自动流程，不阻挡 LoRa
	      static uint8_t stable_done = 0;

	      if (!stable_done)
	      {
	          if (HAL_GetTick() - g_last_poweron_tick < NTN_STABLE_MS)
	          {
	              HAL_Delay(10);
	              // 不要 continue，也可以；但这里 continue 没问题，因为只会持续15秒

	          }
	          stable_done = 1;
	      }


	      // C) 第一步：拉配置（cfg_ready==0 才拉）
	 	     if (g_ntn_cfg_ready == 0 && HAL_GetTick() >= next_try_tick)
	 	     {
	 	         g_ntn_cfg_ready  = 0;
	 	         g_ntn_hello_done = 0;
	 	         ready_test_sent  = 0;

	 	         NTN_SendTest();   // 阻塞拉 cfg

	 	         if (g_ntn_cfg_ready)
	 	         {
	 	             printf("[MAIN] immediate HELLO after CFG\r\n");

	 	             int ok = NTN_SendHelloUsingCfg();
	 	             printf("[MAIN] HELLO result = %d\r\n", ok);

	 	             if (ok)
	 	             {
	 	                 g_ntn_hello_done = 1;
	 	                 printf("HELLO OK -> READY\r\n");
	 	             }
	 	             else
	 	             {
	 	                 g_ntn_cfg_ready  = 0;
	 	                 g_ntn_hello_done = 0;
	 	                 next_try_tick = HAL_GetTick() + 5000;
	 	             }
	 	         }

	 	         next_try_tick = HAL_GetTick() + 20000;
	 	     }


	      // D) 第二步：cfg_ready 之后发 hello
	 	    // D) cfg_ready 之后发 hello：cfg ready 立刻尝试一次，失败再每 5s 重试
	 	    static uint32_t last_hello_try = 0;
	 	    static uint8_t  hello_tried_once_after_cfg = 0;   // 0=还没在本轮cfg ready后尝试过

	 	    // 如果 cfg 失效了，要把一次性标志复位
	 	    if (!g_ntn_cfg_ready) {
	 	        hello_tried_once_after_cfg = 0;
	 	    }

	 	    if (g_ntn_cfg_ready && !g_ntn_hello_done)
	 	    {
	 	        uint32_t now = HAL_GetTick();

	 	        int should_try = 0;

	 	        // ✅ 1) cfg ready 后，立刻尝试一次（不等 5s）
	 	        if (!hello_tried_once_after_cfg) {
	 	            should_try = 1;
	 	            hello_tried_once_after_cfg = 1;
	 	        }
	 	        // ✅ 2) 后续失败：每 5 秒重试
	 	        else if (now - last_hello_try >= 5000) {
	 	            should_try = 1;
	 	        }

	 	        if (should_try)
	 	        {
	 	            last_hello_try = now;

	 	            printf("[MAIN] HELLO using CFG...\r\n");
	 	            int ok = NTN_SendHelloUsingCfg();
	 	            printf("[MAIN] HELLO result = %d\r\n", ok);

	 	            if (ok)
	 	            {
	 	                g_ntn_hello_done = 1;
	 	                printf("HELLO OK -> READY\r\n");

	 	                // ✅ 成功后复位节流计时（可选）
	 	                last_hello_try = 0;
	 	            }
	 	            else
	 	            {
	 	                // ✅ 如果 hello 失败，建议让系统重新拉 cfg（尤其遇到 CME8002/timeout）
	 	                // 如果你的 NTN_SendHelloUsingCfg 内部已经会把 cfg_ready=0，那这里不会重复影响
	 	                g_ntn_cfg_ready = 0;
	 	                // 同时也可以确保复用 socket 不再被使用（保险）
	 	               NTN_Invalidate_UserSock_AndCfg("[MAIN] HELLO failed -> invalidate cfg");


	 	                printf("[MAIN] HELLO failed -> invalidate CFG, refetch\r\n");
	 	            }
	 	        }
	 	    }


	      // E) RS485 周期任务
	      uint32_t now = HAL_GetTick();
	      if (!wind_active && now - last_tick >= 2000)
	      {
	          RS485_WindQuery_Start();
	          last_tick = now;
	      }

	      // F) READY 后 flush LoRa 队列（节流）
	      static uint32_t last_flush = 0;
	      if (g_ntn_cfg_ready && g_ntn_hello_done)
	      {
	          if (HAL_GetTick() - last_flush >= 1000)
	         {
	              last_flush = HAL_GetTick();
	              NTN_FlushLoRaQueue();
	          }
	      }



	      HAL_Delay(1); // 放最后，给一点 CPU 让步


    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLLMUL_3;
  RCC_OscInitStruct.PLL.PLLDIV = RCC_PLLDIV_2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USART1|RCC_PERIPHCLK_USART2
                              |RCC_PERIPHCLK_LPUART1;
  PeriphClkInit.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK2;
  PeriphClkInit.Usart2ClockSelection = RCC_USART2CLKSOURCE_PCLK1;
  PeriphClkInit.Lpuart1ClockSelection = RCC_LPUART1CLKSOURCE_PCLK1;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

// 串口中断回调：做透传桥

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    // PC -> MCU (LPUART1)
    if (huart->Instance == LPUART1)
    {
        uint8_t ch = lpuart_rx;

        if (pc_cmd_idx < sizeof(pc_cmd_buf))
        {
            pc_cmd_buf[pc_cmd_idx++] = ch;

            // CRLF: 0x0D 0x0A
            if (ch == 0x0A && pc_cmd_idx >= 2 &&
                pc_cmd_buf[pc_cmd_idx - 2] == 0x0D)
            {
                pc_cmd_ready = 1;
            }
        }
        else
        {
            pc_cmd_idx   = 0;
            pc_cmd_ready = 0;
        }

        HAL_UART_Receive_IT(&hlpuart1, &lpuart_rx, 1);
        return;
    }

    // RS485 -> MCU (USART2)
    if (huart->Instance == USART2)
    {
        RS485_UART_RxHandler();
        HAL_UART_Receive_IT(&huart2, &rs485_rx_byte, 1);
        return;
    }

    // NTN -> MCU (USART1)
    if (huart->Instance == USART1)
    {
        // 1) 实时回显到 PC
    	if (hlpuart1.gState == HAL_UART_STATE_READY)
    	{
    	    (void)HAL_UART_Transmit(&hlpuart1, &ntn_rx, 1, 0);
    	}

        // 2) 缓存到 ntn_rx_buf（给 toNTN.c 的解析使用）
        if (ntn_rx_len < (int)sizeof(ntn_rx_buf) - 1)
        {
            ntn_rx_buf[ntn_rx_len++] = ntn_rx;
            ntn_rx_buf[ntn_rx_len]   = '\0';
        }

        // ✅ 3) POWERON 检测：只检查“当前行”，不要 strstr 大缓冲
        if (ntn_line_idx < sizeof(ntn_line) - 1)
        {
            ntn_line[ntn_line_idx++] = (char)ntn_rx;
            ntn_line[ntn_line_idx] = '\0';
        }

        if (ntn_rx == '\n')
        {
            if (strstr(ntn_line, "+POWERON:"))
            {
                g_last_poweron_tick = HAL_GetTick();
                HAL_UART_Transmit(&hlpuart1,
                                  (uint8_t*)"[NTN] POWERON detected\r\n",
                                  strlen("[NTN] POWERON detected\r\n"), 100);
            }

            // 行结束，清掉行缓冲
            ntn_line_idx = 0;
            ntn_line[0]  = '\0';
        }


        HAL_UART_Receive_IT(&huart1, &ntn_rx, 1);
        return;
    }
}



void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_8)
    {
    	LoraRx_IrqNotify();
    }
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
