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
#include "adc.h"
#include "dma.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"
#include "semphr.h"
#include "stm32f4xx_hal.h"
#include "timers.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdbool.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

// Enums and Structs
typedef enum {
  BOP, TWIST, PULL
} State;

typedef struct {
  uint32_t player1_time;
  uint32_t player2_time;
} Time_Result;

// Arguments handed to each player task (xTaskCreate pvParameters)
typedef struct {
  QueueHandle_t inputQueue;   // ISR -> player
  QueueHandle_t resultQueue;  // player -> IO manager
} Player_Args;

// Game settings
#define TWIST_ENABLED   1      // 0 = TWIST is never asked (only BOP and PULL), e.g. to test without the pots
#define ROUND_PAUSE_MS  1000   // pause before every round so the players can get ready
#define DEBOUNCE_MS     30     // after a button/switch edge, further edges on that pin are ignored for this long
#define TWIST_THRESHOLD 1000   // ADC counts (of 4095) a pot must move from its position at the cue
#define POT_POLL_MS     1      // how often the pots are checked

// Task settings: priorities are CMSIS values (24 = normal, 32 = above normal), stacks are in 4-byte words
#define PRIO_PLAYER     osPriorityAboveNormal
#define PRIO_IO         osPriorityAboveNormal
#define PRIO_ROUND      osPriorityNormal
#define PRIO_SCORE      osPriorityBelowNormal
#define PRIO_POT        osPriorityAboveNormal
#define STACK_WORDS     256

// semaphores and mutexes

static SemaphoreHandle_t gameStateMutex;
static State gameState;

static const size_t SCORE_THRESHOLD = 9;   // first to this score wins (max 9: one display digit)
static size_t player1_score = 0; 
static size_t player2_score = 0;

// Handles for timers

// Handles for tasks
TaskHandle_t round_manager_handle = NULL;
TaskHandle_t io_manager_handle = NULL;
TaskHandle_t player1_handle = NULL;
TaskHandle_t player2_handle = NULL;
TaskHandle_t score_manager_handle = NULL;
TaskHandle_t pot_monitor_handle = NULL;

// Handles for queues

// from ISR to players
QueueHandle_t player1_input;
QueueHandle_t player2_input;

// from players to Io Manager
QueueHandle_t player1_result;
QueueHandle_t player2_result;

QueueHandle_t io_manager_result;


// Enum aliases for various pins.

// A pin is a port and a pin number. The macros come from main.h (CubeMX user labels).
typedef struct {
  GPIO_TypeDef *port;
  uint16_t pin;
} Pin;

// Indicator lights. Each light is shared by both players (their LEDs are wired together),
// so only the player 1 pins are used.
static const Pin led_twist = { LED_POT_P1_GPIO_Port, LED_POT_P1_Pin };   // D14
static const Pin led_bop   = { LED_BTN_P1_GPIO_Port, LED_BTN_P1_Pin };   // D9
static const Pin led_pull  = { LED_SW_P1_GPIO_Port,  LED_SW_P1_Pin };    // D15

// Inputs. The callback only needs the pin number.
// TWIST is the pot: it is read through the ADC (PA0 / PA1), it has no interrupt pin (0 = none).
// Inputs Player 1
static const uint16_t pin_twist1 = 0;
static const uint16_t pin_bop1   = BTN_P1_Pin;   // PA10 (D2)
static const uint16_t pin_pull1  = SW_P1_Pin;    // PB4  (D5)

// Inputs player 2
static const uint16_t pin_twist2 = 0;
static const uint16_t pin_bop2   = BTN_P2_Pin;   // PA8  (D7)
static const uint16_t pin_pull2  = SW_P2_Pin;    // PA9  (D8)

// 7 seg displays for score
uint16_t pin_score;
// uint16_t pin_score1;
// uint16_t pin_score2;

// Buzzer
uint16_t pin_buzzer;

/* USER CODE END PV */

// xorshift32 pseudo random generator. It keeps its state between calls.
static uint32_t random_state = 0;

uint32_t random_next(void)
{
  if (random_state == 0) {
    // seed once: tick count plus the position inside the current tick (never 0)
    random_state = (HAL_GetTick() ^ (SysTick->VAL << 8)) | 1u;
  }
  uint32_t x = random_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  random_state = x;
  return x;
}

State get_random_gameState() {
  // the states that can be asked (TWIST only once the pot is implemented)
  static const State playable[] = {
    BOP,
#if TWIST_ENABLED
    TWIST,
#endif
    PULL
  };
  return playable[random_next() % (sizeof(playable) / sizeof(playable[0]))];
}

void buzz_once()
{
  //TODO: buzzer implementation
}

void led_write(const Pin *led, GPIO_PinState state)
{
  HAL_GPIO_WritePin(led->port, led->pin, state);
}

void all_leds_off()
{
  led_write(&led_twist, GPIO_PIN_RESET);
  led_write(&led_bop, GPIO_PIN_RESET);
  led_write(&led_pull, GPIO_PIN_RESET);
}

// ---- TWIST: the pots ----
// The ADC + DMA keep pot_raw up to date (pot_raw[0] = player 1 on PA0, pot_raw[1] = player 2 on PA1).
// A pot has no interrupt, so pot_monitor() checks it every POT_POLL_MS and puts a TWIST message into the
// player's input queue, exactly like the EXTI callback does for a button. The players need no special code.
static volatile uint16_t pot_raw[2];
static volatile uint16_t pot_ref[2];      // pot position at the cue
static volatile bool pot_armed = false;   // true while a TWIST round is running
static volatile bool pot_fired[2];        // this pot already reported its move in this round

// remember where both pots are now (called by the IO manager at the cue of a TWIST round)
void pot_arm(void)
{
  pot_ref[0] = pot_raw[0];
  pot_ref[1] = pot_raw[1];
  pot_fired[0] = false;
  pot_fired[1] = false;
  pot_armed = true;
}

void pot_disarm(void)
{
  pot_armed = false;
}

void pot_monitor(void *args)
{
  (void)args;
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(POT_POLL_MS));
    if (!pot_armed) {
      continue;
    }

    for (int p = 0; p < 2; p++) {
      int32_t delta = (int32_t)pot_raw[p] - (int32_t)pot_ref[p];
      if (delta < 0) {
        delta = -delta;
      }
      // moved far enough (any direction), and not reported yet
      if (!pot_fired[p] && delta >= TWIST_THRESHOLD) {
        pot_fired[p] = true;
        State state = TWIST;
        xQueueSend(p == 0 ? player1_input : player2_input, &state, 0);
      }
    }
  }
}

void round_manager(void *args) {
  // initialize all relevant registers, pins, and variables
  for (;;) {

    // Turn off all LEDs
    all_leds_off();

    // short pause so the players can get ready
    vTaskDelay(pdMS_TO_TICKS(ROUND_PAUSE_MS));

    // reset inputs and outputs (except scores)
    // buzz once
    buzz_once();

    // start I/O manager
    xTaskNotifyGive(io_manager_handle);

    // wait for I/O manager with timing results (the result queue is the only signal)
    Time_Result timeResult;
    xQueueReceive(io_manager_result, &timeResult, portMAX_DELAY);

    // determine winner
    uint32_t p1_time = timeResult.player1_time;
    uint32_t p2_time = timeResult.player2_time;
    
    //TODO: store timings somewhere

    if (p1_time < p2_time) {
      player1_score++;
    } else {
      player2_score++;
    }

    //TODO: display scores
    // update scores/score manager

    // if winner threshold reached

    if (player1_score >= SCORE_THRESHOLD) {
      //TODO: winstate
    } else if (player2_score >= SCORE_THRESHOLD) {
      // TODO: winstate
    }
    // enter winstate
  }
}


void io_manager(void *args) 
{
  uint32_t player1_value;
  uint32_t player2_value;

  for (;;) {
    // Wait until start notification
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    
    // pick a random game state.
    if (xSemaphoreTake(gameStateMutex, portMAX_DELAY) == pdTRUE) {
      gameState = get_random_gameState();
      xSemaphoreGive(gameStateMutex);
    }

    // Throw away presses from before the cue (early presses, contact bounce of the last round)
    xQueueReset(player1_input);
    xQueueReset(player2_input);

    // Turn on the correct LEDs

    switch (gameState) {
      case BOP:
        led_write(&led_bop, GPIO_PIN_SET);
      break;
      case TWIST:
        led_write(&led_twist, GPIO_PIN_SET);
      break;
      case PULL:
        led_write(&led_pull, GPIO_PIN_SET);
      break;
    }

    // TWIST: remember where the pots are at the cue
    if (gameState == TWIST) {
      pot_arm();
    }

    // start player tasks 1 and 2
    xTaskNotifyGive(player1_handle);
    xTaskNotifyGive(player2_handle);
    // wait for both players
    xQueueReceive(player1_result, &player1_value, portMAX_DELAY);
    xQueueReceive(player2_result, &player2_value, portMAX_DELAY);
    pot_disarm();
    
    all_leds_off();


    // determine winner. 1 if p1 wins, 2 if p2 wins.
    // a draw is assumed to be impossible
    // uint32_t winner = 1 + (player2_value  < player1_value);

    buzz_once();
    

    
    
    // winner indication (buzz, flash LEDs?)
    // notify round manager with both reaction times
    Time_Result result = { player1_value, player2_value };
    xQueueSend(io_manager_result, &result, portMAX_DELAY);
  }
}

void player(void *args)
{

  Player_Args *player_args = (Player_Args *)args;
  QueueHandle_t inputQueue = player_args->inputQueue;
  QueueHandle_t resultQueue = player_args->resultQueue;
  State receivedState;

  for (;;) {
    // wait until IO manager notifies start
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t start_time = HAL_GetTick();
    // wait for input

    while(true) {
      // wait for ISR message
      if(xQueueReceive(inputQueue, &receivedState, portMAX_DELAY) == pdPASS) {
        if (xSemaphoreTake(gameStateMutex, portMAX_DELAY) == pdTRUE) {
          // correct input
          if (receivedState == gameState) { 
            uint32_t end_time = HAL_GetTick() - start_time;
            xQueueOverwrite(resultQueue, &end_time);
            xSemaphoreGive(gameStateMutex); // release mutex
            break; // exit while loop and go back to initial waiting state again
          }
          // release mutex, go back to waiting for input
          xSemaphoreGive(gameStateMutex);
        }
      }
    }
  }
}

void score_manager(void *args) {
  for (;;) {
    // wait until notification is received
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // read score mutex 
    // update 7 seg displays
  }
}


// Creates the mutex, the queues and the tasks. Called once from main(), before the scheduler starts.
static Player_Args player1_args;
static Player_Args player2_args;

void game_init(void)
{
  // ADC + DMA (circular) keep pot_raw up to date. The DMA interrupt is not needed and would fire
  // about 200 000 times per second, so it is switched off in the NVIC (the transfers keep running).
  if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)pot_raw, 2) != HAL_OK) {
    Error_Handler();
  }
  HAL_NVIC_DisableIRQ(DMA2_Stream0_IRQn);

  gameStateMutex = xSemaphoreCreateMutex();

  // ISR -> players: one State per button press, a few can wait
  player1_input = xQueueCreate(8, sizeof(State));
  player2_input = xQueueCreate(8, sizeof(State));
  // players -> IO manager: only the latest reaction time (xQueueOverwrite needs length 1)
  player1_result = xQueueCreate(1, sizeof(uint32_t));
  player2_result = xQueueCreate(1, sizeof(uint32_t));
  // IO manager -> round manager
  io_manager_result = xQueueCreate(1, sizeof(Time_Result));

  if (gameStateMutex == NULL || player1_input == NULL || player2_input == NULL ||
      player1_result == NULL || player2_result == NULL || io_manager_result == NULL) {
    Error_Handler();
  }

  player1_args.inputQueue = player1_input;
  player1_args.resultQueue = player1_result;
  player2_args.inputQueue = player2_input;
  player2_args.resultQueue = player2_result;

  if (xTaskCreate(io_manager, "io_manager", STACK_WORDS, NULL, PRIO_IO, &io_manager_handle) != pdPASS ||
      xTaskCreate(player, "player1", STACK_WORDS, &player1_args, PRIO_PLAYER, &player1_handle) != pdPASS ||
      xTaskCreate(player, "player2", STACK_WORDS, &player2_args, PRIO_PLAYER, &player2_handle) != pdPASS ||
      xTaskCreate(score_manager, "score_manager", STACK_WORDS, NULL, PRIO_SCORE, &score_manager_handle) != pdPASS ||
      xTaskCreate(round_manager, "round_manager", STACK_WORDS, NULL, PRIO_ROUND, &round_manager_handle) != pdPASS ||
      xTaskCreate(pot_monitor, "pot_monitor", 128, NULL, PRIO_POT, &pot_monitor_handle) != pdPASS) {
    Error_Handler();
  }
}

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
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
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_SPI1_Init();
  MX_USART2_UART_Init();
  MX_TIM2_Init();
  MX_TIM4_Init();
  /* USER CODE BEGIN 2 */
  game_init();
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

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
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM11 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM11)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    QueueHandle_t queue = NULL;
    State state;
    // Player 1
    if (GPIO_Pin == pin_bop1) {
        queue = player1_input;
        state = BOP;
    } else if (GPIO_Pin == pin_twist1) {
        queue = player1_input;
        state = TWIST;
    } else if (GPIO_Pin == pin_pull1) {
        queue = player1_input;
        state = PULL;
    // Player 2
    } else if (GPIO_Pin == pin_bop2) {
        queue = player2_input;
        state = BOP;
    } else if (GPIO_Pin == pin_twist2) {
        queue = player2_input;
        state = TWIST;
    } else if (GPIO_Pin == pin_pull2) {
        queue = player2_input;
        state = PULL;
    } else {
        return;  // Irrelevant
    }

    // Debounce: the first edge is the real press. A contact bounces for a few ms, so ignore
    // further edges on the same pin for DEBOUNCE_MS (no delay is added to the real press).
    static uint32_t last_edge_ms[16];
    uint32_t line = __builtin_ctz(GPIO_Pin);
    uint32_t now = HAL_GetTick();
    if ((now - last_edge_ms[line]) < DEBOUNCE_MS) {
      return;
    }
    last_edge_ms[line] = now;

    if (queue != NULL) {
      xQueueSendFromISR(queue, &state, &higher_priority_task_woken);
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
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
