/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
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
#include "cmsis_os.h"
#include "lwip.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lwip/sockets.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct
{
  float temperature;
  float humidity;
  float air_quality;
  uint32_t timestamp;
} SensorData_t;

/* 共享设备状态：用互斥锁保护 */
typedef struct
{
  float alarm_threshold;
  SensorData_t latest;
  int has_latest;
} DeviceState_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
SPI_HandleTypeDef hspi5;

UART_HandleTypeDef huart1;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for SensorTask */
osThreadId_t SensorTaskHandle;
const osThreadAttr_t SensorTask_attributes = {
  .name = "SensorTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for NetworkTask */
osThreadId_t NetworkTaskHandle;
const osThreadAttr_t NetworkTask_attributes = {
  .name = "NetworkTask",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
/* Definitions for SensorDataQueue */
osMessageQueueId_t SensorDataQueueHandle;
const osMessageQueueAttr_t SensorDataQueue_attributes = {
  .name = "SensorDataQueue"
};
/* USER CODE BEGIN PV */
volatile int lwip_ready = 0;
DeviceState_t g_state = { 26.5f, {0.0f, 0.0f, 0.0f, 0u}, 0 };
osMutexId_t g_state_mutex = NULL;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI5_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USB_OTG_FS_USB_Init(void);
void StartDefaultTask(void *argument);
void StartSensorTask(void *argument);
void StartNetworkTask(void *argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* MicroLIB 的 printf 不支持 %f，这里把 float 转成 "xx.x" 文本 */
static void ftoa_1(char *buf, float v)
{
  int ip = (int)v;
  int dp = (int)((v - (float)ip) * 10.0f + 0.5f);
  if (dp < 0)
  {
    dp = -dp;
  }
  if (dp >= 10)
  {
    dp = 0;
    ip++;
  }
  sprintf(buf, "%d.%d", ip, dp);
}

/* ===== 命令处理：函数指针 + 命令表 ===== */
#define FW_VERSION "1.0.0"

typedef struct
{
  const char *name;
  void (*handler)(int conn_fd, const char *args);
} CommandEntry_t;

static void send_str(int conn_fd, const char *s)
{
  lwip_send(conn_fd, s, strlen(s), 0);
}

static void cmd_version(int conn_fd, const char *args)
{
  (void)args;
  send_str(conn_fd, "OTA v" FW_VERSION "\r\n");
}

static void cmd_uptime(int conn_fd, const char *args)
{
  char buf[32];
  (void)args;
  sprintf(buf, "UPTIME=%lu s\r\n", (unsigned long)(HAL_GetTick() / 1000u));
  send_str(conn_fd, buf);
}

static void cmd_sensor(int conn_fd, const char *args)
{
  char buf[96], t[8], h[8], a[8];
  SensorData_t d;
  int has;
  (void)args;

  osMutexAcquire(g_state_mutex, osWaitForever);
  d = g_state.latest;
  has = g_state.has_latest;
  osMutexRelease(g_state_mutex);

  if (!has)
  {
    send_str(conn_fd, "SENSOR: no data yet\r\n");
    return;
  }

  ftoa_1(t, d.temperature);
  ftoa_1(h, d.humidity);
  ftoa_1(a, d.air_quality);
  sprintf(buf, "SENSOR T=%s H=%s A=%s TS=%lu\r\n", t, h, a, (unsigned long)d.timestamp);
  send_str(conn_fd, buf);
}

static void cmd_status(int conn_fd, const char *args)
{
  char buf[64];
  (void)args;
  sprintf(buf, "IP=192.168.31.20 PORT=5000 UPTIME=%lu s\r\n",
          (unsigned long)(HAL_GetTick() / 1000u));
  send_str(conn_fd, buf);
}

static void cmd_set_alarm(int conn_fd, const char *args)
{
  char buf[32];
  int val;

  if (args == NULL || *args == '\0')
  {
    send_str(conn_fd, "USAGE: SET_ALARM <temp>\r\n");
    return;
  }

  val = atoi(args);
  osMutexAcquire(g_state_mutex, osWaitForever);
  g_state.alarm_threshold = (float)val;
  osMutexRelease(g_state_mutex);

  sprintf(buf, "ALARM set to %d C\r\n", val);
  send_str(conn_fd, buf);
}

static void cmd_led_on(int conn_fd, const char *args)
{
  (void)args;
  HAL_GPIO_WritePin(LED_RED_GPIO_Port, LED_RED_Pin, GPIO_PIN_RESET);
  send_str(conn_fd, "LED ON\r\n");
}

static void cmd_led_off(int conn_fd, const char *args)
{
  (void)args;
  HAL_GPIO_WritePin(LED_RED_GPIO_Port, LED_RED_Pin, GPIO_PIN_SET);
  send_str(conn_fd, "LED OFF\r\n");
}

static const CommandEntry_t cmd_table[] =
{
  { "VERSION",   cmd_version },
  { "UPTIME",    cmd_uptime },
  { "SENSOR",    cmd_sensor },
  { "STATUS",    cmd_status },
  { "SET_ALARM", cmd_set_alarm },
  { "LED_ON",    cmd_led_on },
  { "LED_OFF",   cmd_led_off },
  { NULL,        NULL }
};

static void command_dispatch(int conn_fd, char *line)
{
  char *cmd, *args = NULL, *sp;
  size_t len = strlen(line);
  uint32_t i;

  /* 去掉行尾 \r / \n */
  while (len > 0u && (line[len - 1u] == '\r' || line[len - 1u] == '\n'))
  {
    line[--len] = '\0';
  }

  cmd = line;
  sp = strchr(cmd, ' ');
  if (sp != NULL)
  {
    *sp = '\0';
    args = sp + 1;
  }

  for (i = 0u; cmd_table[i].name != NULL; i++)
  {
    if (strcmp(cmd_table[i].name, cmd) == 0)
    {
      cmd_table[i].handler(conn_fd, args);
      return;
    }
  }

  send_str(conn_fd, "ERR unknown command\r\n");
}
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
  MX_SPI5_Init();
  MX_USART1_UART_Init();
  MX_USB_OTG_FS_USB_Init();
  /* USER CODE BEGIN 2 */
  printf("OTA board boot OK\r\n");
  printf("SYSCLK = %lu MHz\r\n", SystemCoreClock / 1000000UL);
  printf("FreeRTOS starting...\r\n");
  HAL_GPIO_WritePin(LED_RED_GPIO_Port, LED_RED_Pin,GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin,GPIO_PIN_SET);
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  g_state_mutex = osMutexNew(NULL);
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of SensorDataQueue */
  SensorDataQueueHandle = osMessageQueueNew (8, 16, &SensorDataQueue_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of SensorTask */
  SensorTaskHandle = osThreadNew(StartSensorTask, NULL, &SensorTask_attributes);

  /* creation of NetworkTask */
  NetworkTaskHandle = osThreadNew(StartNetworkTask, NULL, &NetworkTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SPI5 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI5_Init(void)
{

  /* USER CODE BEGIN SPI5_Init 0 */

  /* USER CODE END SPI5_Init 0 */

  /* USER CODE BEGIN SPI5_Init 1 */

  /* USER CODE END SPI5_Init 1 */
  /* SPI5 parameter configuration*/
  hspi5.Instance = SPI5;
  hspi5.Init.Mode = SPI_MODE_MASTER;
  hspi5.Init.Direction = SPI_DIRECTION_2LINES;
  hspi5.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi5.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi5.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi5.Init.NSS = SPI_NSS_SOFT;
  hspi5.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi5.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi5.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi5.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi5.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi5) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI5_Init 2 */

  /* USER CODE END SPI5_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USB_OTG_FS Initialization Function
  * @param None
  * @retval None
  */
static void MX_USB_OTG_FS_USB_Init(void)
{

  /* USER CODE BEGIN USB_OTG_FS_Init 0 */

  /* USER CODE END USB_OTG_FS_Init 0 */

  /* USER CODE BEGIN USB_OTG_FS_Init 1 */

  /* USER CODE END USB_OTG_FS_Init 1 */
  /* USER CODE BEGIN USB_OTG_FS_Init 2 */

  /* USER CODE END USB_OTG_FS_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(SPI5_CS_GPIO_Port, SPI5_CS_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, LED_GREEN_Pin|LED_RED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : KEY2_Pin */
  GPIO_InitStruct.Pin = KEY2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(KEY2_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : SPI5_CS_Pin */
  GPIO_InitStruct.Pin = SPI5_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(SPI5_CS_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : KEY_UP_Pin */
  GPIO_InitStruct.Pin = KEY_UP_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(KEY_UP_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : KEY1_Pin KEY0_Pin */
  GPIO_InitStruct.Pin = KEY1_Pin|KEY0_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOH, &GPIO_InitStruct);

  /*Configure GPIO pins : LED_GREEN_Pin LED_RED_Pin */
  GPIO_InitStruct.Pin = LED_GREEN_Pin|LED_RED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : PA10 PA11 PA12 */
  GPIO_InitStruct.Pin = GPIO_PIN_10|GPIO_PIN_11|GPIO_PIN_12;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF10_OTG_FS;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
int fputc(int ch, FILE *f)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
  return ch;
}
/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* init code for LWIP */
  MX_LWIP_Init();
  /* USER CODE BEGIN 5 */
  lwip_ready = 1;
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END 5 */
}

/* USER CODE BEGIN Header_StartSensorTask */
/**
* @brief Function implementing the SensorTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartSensorTask */
void StartSensorTask(void *argument)
{
  /* USER CODE BEGIN StartSensorTask */
  SensorData_t data;
  float temp_hist[5] = {0.0f};
  float humi_hist[5] = {0.0f};
  float temp_raw, humi_raw, air_raw;
  float temp_sum, humi_sum;
  float alarm_thr;
  uint32_t rng = 12345u;
  uint32_t idx = 0u;
  uint32_t i;

  for(;;)
  {
    /* 模拟传感器原始值（真实项目里这里替换成实际传感器读取） */
    rng = rng * 1103515245u + 12345u;
    temp_raw = 25.0f + 2.0f * ((float)((rng >> 16) & 0x7FFFu) / 32767.0f);
    rng = rng * 1103515245u + 12345u;
    humi_raw = 55.0f + 10.0f * ((float)((rng >> 16) & 0x7FFFu) / 32767.0f);
    rng = rng * 1103515245u + 12345u;
    air_raw  = 60.0f + 40.0f * ((float)((rng >> 16) & 0x7FFFu) / 32767.0f);

    /* 滑动平均滤波（窗口 5） */
    temp_hist[idx % 5u] = temp_raw;
    humi_hist[idx % 5u] = humi_raw;
    temp_sum = 0.0f;
    humi_sum = 0.0f;
    for (i = 0u; i < 5u; i++)
    {
      temp_sum += temp_hist[i];
      humi_sum += humi_hist[i];
    }

    data.temperature = temp_sum / 5.0f;
    data.humidity    = humi_sum / 5.0f;
    data.air_quality = air_raw;
    data.timestamp   = HAL_GetTick() / 1000u;

    /* 更新共享状态，并读取报警阈值（互斥锁保护） */
    osMutexAcquire(g_state_mutex, osWaitForever);
    g_state.latest = data;
    g_state.has_latest = 1;
    alarm_thr = g_state.alarm_threshold;
    osMutexRelease(g_state_mutex);

    /* 阈值判断 */
    if (data.temperature > alarm_thr)
    {
      printf("[SENSOR] high temperature alarm!\r\n");
    }

    /* 放进消息队列，交给 NetworkTask */
    if (osMessageQueuePut(SensorDataQueueHandle, &data, 0u, 0u) != osOK)
    {
      printf("[SENSOR] queue full\r\n");
    }

    idx++;
    osDelay(5000);
  }
  /* USER CODE END StartSensorTask */
}

/* USER CODE BEGIN Header_StartNetworkTask */
/**
* @brief Function implementing the NetworkTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartNetworkTask */
void StartNetworkTask(void *argument)
{
  /* USER CODE BEGIN StartNetworkTask */
  int listen_fd = -1, conn_fd = -1, udp_fd = -1;
  struct sockaddr_in server_addr, client_addr, udp_dst;
  ip4_addr_t udp_ip;
  socklen_t client_addr_len = sizeof(client_addr);
  char rx_buf[256];
  char tx_line[128];
  char t_str[8], h_str[8], a_str[8];
  int rx_len, tx_len, flags;
  SensorData_t sensor_data;

  /* 等待 LwIP 初始化完成（defaultTask 里 MX_LWIP_Init 后会置位） */
  while (lwip_ready == 0)
  {
    osDelay(10);
  }

  /* 建立 TCP 监听 socket，失败就重试 */
  for(;;)
  {
    listen_fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
      printf("[NET] socket() error %d\r\n", listen_fd);
      osDelay(1000);
      continue;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(5000);

    if (lwip_bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
      printf("[NET] bind() error\r\n");
      lwip_close(listen_fd);
      osDelay(1000);
      continue;
    }

    if (lwip_listen(listen_fd, 1) < 0)
    {
      printf("[NET] listen() error\r\n");
      lwip_close(listen_fd);
      osDelay(1000);
      continue;
    }

    /* 把监听 socket 设为非阻塞 */
    flags = lwip_fcntl(listen_fd, F_GETFL, 0);
    lwip_fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    printf("[NET] TCP server ready, port 5000\r\n");
    break;
  }

  /* 创建 UDP socket，用于周期上报传感器数据（目标：电脑 192.168.31.121:6000） */
  udp_fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
  if (udp_fd >= 0)
  {
    memset(&udp_dst, 0, sizeof(udp_dst));
    udp_dst.sin_family = AF_INET;
    IP4_ADDR(&udp_ip, 192, 168, 31, 121);
    udp_dst.sin_addr.s_addr = udp_ip.addr;
    udp_dst.sin_port = htons(6000);
    printf("[NET] UDP report target 192.168.31.121:6000\r\n");
  }
  else
  {
    printf("[NET] UDP socket() error %d\r\n", udp_fd);
  }

  /* 事件循环：接受连接 + 处理命令 + 通过 UDP 上报传感器数据 */
  for(;;)
  {
    /* 1. 接受连接（非阻塞） */
    if (conn_fd < 0)
    {
      conn_fd = lwip_accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_len);
      if (conn_fd >= 0)
      {
        printf("[NET] client connected\r\n");
        flags = lwip_fcntl(conn_fd, F_GETFL, 0);
        lwip_fcntl(conn_fd, F_SETFL, flags | O_NONBLOCK);
      }
    }

    /* 2. 收命令（非阻塞） */
    if (conn_fd >= 0)
    {
      rx_len = lwip_recv(conn_fd, rx_buf, sizeof(rx_buf) - 1, 0);
      if (rx_len > 0)
      {
        rx_buf[rx_len] = '\0';
        printf("[NET] RX(%d): %s\r\n", rx_len, rx_buf);

        command_dispatch(conn_fd, rx_buf);
      }
      else if (rx_len == 0)
      {
        lwip_close(conn_fd);
        conn_fd = -1;
        printf("[NET] client disconnected\r\n");
      }
      /* rx_len < 0：非阻塞模式下无数据，忽略 */
    }

    /* 3. 从队列取传感器数据，通过 UDP 上报 */
    if (osMessageQueueGet(SensorDataQueueHandle, &sensor_data, NULL, 0u) == osOK)
    {
      ftoa_1(t_str, sensor_data.temperature);
      ftoa_1(h_str, sensor_data.humidity);
      ftoa_1(a_str, sensor_data.air_quality);
      tx_len = sprintf(tx_line, "T=%s H=%s A=%s TS=%lu\r\n",
                       t_str, h_str, a_str, (unsigned long)sensor_data.timestamp);
      if (udp_fd >= 0)
      {
        lwip_sendto(udp_fd, tx_line, tx_len, 0,
                    (struct sockaddr *)&udp_dst, sizeof(udp_dst));
      }
      printf("[NET] UDP report: %s", tx_line);
    }

    osDelay(10);
  }
  /* USER CODE END StartNetworkTask */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

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
#ifdef USE_FULL_ASSERT
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
