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

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

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

// Results of both players at the end of a round
typedef struct {
  uint32_t player1_time;
  uint32_t player2_time;
} Time_Result;

// Arguments handed to each player task on creation
typedef struct {
  QueueHandle_t inputQueue;   // ISR -> player
  QueueHandle_t resultQueue;  // player -> IO manager
} Player_Args;

// Game settings
#define SCORE_THRESHOLD 9
#define ROUND_PAUSE_MS  1000   // pause before every round so the players can get ready
#define DEBOUNCE_MS     30     // after a button/switch edge, further edges on that pin are ignored for this long
#define TWIST_THRESHOLD 1000   // ADC counts (of 4095) a pot must move from its position at the cue
#define POT_POLL_MS     1      // how often the pots are checked

// Round timing
#define ROUND_TIMEOUT_START_MS  3000
#define ROUND_TIMEOUT_STEP_MS   100   // shorten a round by this amount
#define ROUND_TIMEOUT_MIN_MS    1000
#define LATE_GRACE_MS           500     // after the limit a correct input is still noticed and reported as "late" (no point). 0 = off
#define NO_REACTION             0xFFFFFFFFu   // "reaction time" of a player who did not react at all

// Outputs
#define SEG_COMMON_ANODE  0     // set to 1 if the 7-segment displays are common anode
// Buzzer default parameters
#define BEEP_HZ           1000
#define BEEP_MS           100

// Task priorities
#define PRIO_PLAYER     osPriorityAboveNormal
#define PRIO_IO         osPriorityAboveNormal
#define PRIO_ROUND      osPriorityNormal
#define PRIO_SCORE      osPriorityBelowNormal
#define PRIO_POT        osPriorityAboveNormal

// Stack settings
#define STACK_WORDS     256
#define STACK_ROUND_WORDS 512   // the round manager formats text for the UART log

// semaphores and mutexes

static SemaphoreHandle_t gameStateMutex;
static State gameState;

static SemaphoreHandle_t scoreMutex;
static size_t player1_score = 0; 
static size_t player2_score = 0;

// best (shortest) on-time reaction of each player, in ms
static uint32_t best_time[2] = { NO_REACTION, NO_REACTION };

// time to react in a round (changed by the round manager)
static volatile uint32_t round_timeout_ms = ROUND_TIMEOUT_START_MS;   

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

// Convenience struct for pin info. Uses CubeMX user labels.
typedef struct {
  GPIO_TypeDef *port;
  uint16_t pin;
} Pin;

// Indicator lights
static const Pin led_twist = { LED_POT_P1_GPIO_Port, LED_POT_P1_Pin };   // D14
static const Pin led_bop   = { LED_BTN_P1_GPIO_Port, LED_BTN_P1_Pin };   // D9
static const Pin led_pull  = { LED_SW_P1_GPIO_Port,  LED_SW_P1_Pin };    // D15

// Inputs Player 1 
static const uint16_t pin_bop1   = BTN_P1_Pin;   // PA10 (D2)
static const uint16_t pin_pull1  = SW_P1_Pin;    // PB4  (D5)

// Inputs player 2
static const uint16_t pin_bop2   = BTN_P2_Pin;   // PA8  (D7)
static const uint16_t pin_pull2  = SW_P2_Pin;    // PA9  (D8)

// 7 seg displays for score
uint16_t pin_score;
// uint16_t pin_score1;
// uint16_t pin_score2;

// Buzzer
uint16_t pin_buzzer;

/* USER CODE END PV */

// Buzzer with 1 MHz timer
void buzzer_tone(uint32_t hz)
{
  if (hz == 0) {
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, 0);          // silent
    return;
  }
  uint32_t period = 1000000u / hz;
  __HAL_TIM_SET_AUTORELOAD(&htim4, period - 1);
  __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, period / 2);   // 50 % duty
}

void buzz(uint32_t hz, uint32_t ms)
{
  buzzer_tone(hz);
  vTaskDelay(pdMS_TO_TICKS(ms));
  buzzer_tone(0);
}

uint32_t random(uint32_t seed)
{
    uint32_t x = seed;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    seed = x;
    return x;
}

State get_random_gameState() {
  size_t state_count = 3;
  return (State) (random(HAL_GetTick() % state_count));
}

void buzz_once()
{
  buzz(BEEP_HZ, BEEP_MS);
}

void led_write(const Pin *led, GPIO_PinState state)
{
  HAL_GPIO_WritePin(led->port, led->pin, state);
}

// Set all LEDs to the same input state
void set_all_leds(GPIO_PinState pinState)
{
  led_write(&led_twist, pinState);
  led_write(&led_bop, pinState);
  led_write(&led_pull, pinState);
}

// 7-segment displays: two 74HC595 shift registers, daisy chained on SPI1 (left chip = player 1, right chip = player 2)
// bit0 = segment a ... bit6 = g (595 outputs QA..QG)
static const uint8_t seg_digit[10] = {
    0xC0, 0xF9, 0xA4, 0xB0, 0x99,
    0x92, 0x82, 0xF8, 0x80, 0x90
};

static uint8_t seg_pattern(size_t digit)
{
  uint8_t v = seg_digit[digit > 9 ? 9 : digit];
#if SEG_COMMON_ANODE
  v = (uint8_t)~v;
#endif
  return v;
}

// Show input values on 7-segment displays
void display_show(size_t score1, size_t score2)
{
  // the first byte clocked out ends up in the last chip of the chain: player 1 (left) goes first
  uint8_t frame[2] = { seg_pattern(score1), seg_pattern(score2) };
  HAL_SPI_Transmit(&hspi1, frame, 2, 10);
  HAL_GPIO_WritePin(LATCH_595_GPIO_Port, LATCH_595_Pin, GPIO_PIN_SET);    // outputs take the new bits
  HAL_GPIO_WritePin(LATCH_595_GPIO_Port, LATCH_595_Pin, GPIO_PIN_RESET);
}

// UART log (115200 baud, ST-LINK virtual COM port).
void uart_printf(const char *fmt, ...)
{
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) {
    HAL_UART_Transmit(&huart2, (uint8_t *)buf, (uint16_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1), 100);
  }
}

// Potentiometer monitoring for Twist.
static volatile uint16_t pot_raw[2];
static volatile uint16_t pot_ref[2];      // pot position at the cue
static volatile bool pot_armed = false;   // true while a TWIST round is running
static volatile bool pot_fired[2];        // this pot already reported its move in this round

// Reset old pot data, set pots to active, store current pot values
void pot_arm(void)
{
  pot_ref[0] = pot_raw[0];
  pot_ref[1] = pot_raw[1];
  pot_fired[0] = false;
  pot_fired[1] = false;
  pot_armed = true;
}

// Disable pot monitoring
void pot_disarm(void)
{
  pot_armed = false;
}

// Repeatedly poll the pots to see if they've been twisted.
void pot_monitor(void *args)
{
  (void)args;
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(POT_POLL_MS));
    if (!pot_armed)
      continue;

    // For both pots..
    for (int p = 0; p < 2; p++) {
      int32_t delta = (int32_t)pot_raw[p] - (int32_t)pot_ref[p];

      if (delta < 0)
        delta = -delta;

      // if moved far enough and not reported yet
      if (!pot_fired[p] && delta >= TWIST_THRESHOLD) {
        // Mark pot as fired and communicate to relevant player
        pot_fired[p] = true;
        State state = TWIST;
        xQueueSend(p == 0 ? player1_input : player2_input, &state, 0);
      }
    }
  }
}

// Time a player has to react in this round. 
// Timeout shrinks every round, down to a minimum
static uint32_t timeout_for_round(uint32_t round)
{
  uint32_t cut = (round - 1) * ROUND_TIMEOUT_STEP_MS;
  uint32_t max_cut = ROUND_TIMEOUT_START_MS - ROUND_TIMEOUT_MIN_MS;
  return (cut >= max_cut) ? ROUND_TIMEOUT_MIN_MS : (ROUND_TIMEOUT_START_MS - cut);
}

typedef enum {
  REACTION_NONE,   // the player did not react at all
  REACTION_LATE,   // correct input, but after the time limit
  REACTION_OK      // correct input in time
} Reaction;

static Reaction classify(uint32_t time_ms, uint32_t timeout_ms)
{
  if (time_ms == NO_REACTION) {
    return REACTION_NONE;
  }
  return (time_ms > timeout_ms) ? REACTION_LATE : REACTION_OK;
}

// Formatted text for the log: "312 ms", "late (3105 ms)" or "none"
static void format_reaction(char *buf, size_t len, uint32_t time_ms, Reaction kind)
{
  switch (kind) {
    case REACTION_OK:
      snprintf(buf, len, "%lu ms", (unsigned long)time_ms);
      break;

    case REACTION_LATE:
      snprintf(buf, len, "late (%lu ms)", (unsigned long)time_ms);
      break;

    default:
      snprintf(buf, len, "none");
      break;
  }
}

// Get name of state as a string
static const char *state_name(State state)
{
  switch (state) {
    case BOP:   return "BOP";
    case TWIST: return "TWIST";
    default:    return "PULL";
  }
}

void round_manager(void *args) {
  for (;;) {

    // wait for blue button
    uart_printf("\r\nBOP-IT: Press the blue button to start (first to %u wins, free heap %u bytes)\r\n",
                (unsigned)SCORE_THRESHOLD, (unsigned)xPortGetFreeHeapSize());
    ulTaskNotifyTake(pdTRUE, 0);              // forget presses from before
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Reset scores and start new game
    xSemaphoreTake(scoreMutex, portMAX_DELAY);
    player1_score = 0;
    player2_score = 0;
    xSemaphoreGive(scoreMutex);


    // Reset best time and score displays
    xTaskNotifyGive(score_manager_handle);
    best_time[0] = NO_REACTION;
    best_time[1] = NO_REACTION;

    int game_winner = 0; // 0 = no winner, game is still running
    for (uint32_t round = 1; game_winner == 0; round++) {

      // Turn off all LEDs
      set_all_leds(GPIO_PIN_RESET);

      // short pause so the players can get ready
      vTaskDelay(pdMS_TO_TICKS(ROUND_PAUSE_MS));

      // time to react in this round (the players read it when they are started)
      round_timeout_ms = timeout_for_round(round);

      // buzz once: the round starts
      buzz_once();

      // start I/O manager
      xTaskNotifyGive(io_manager_handle);

      // wait for I/O manager with timing results (the result queue is the only signal)
      Time_Result timeResult;
      xQueueReceive(io_manager_result, &timeResult, portMAX_DELAY);

      // classify both reactions: in time, late (after the limit) or none at all
      uint32_t p1_time = timeResult.player1_time;
      uint32_t p2_time = timeResult.player2_time;
      Reaction r1 = classify(p1_time, round_timeout_ms);
      Reaction r2 = classify(p2_time, round_timeout_ms);

      // determine winner: the faster of the players who were in time. Late or no reaction = no point.
      int winner = 0;                         // 0 = nobody scores this round
      if (r1 == REACTION_OK && r2 == REACTION_OK) {
        winner = (p1_time < p2_time) ? 1 : 2; // equal times: player 2 (ties are not handled yet)
      } else if (r1 == REACTION_OK) {
        winner = 1;
      } else if (r2 == REACTION_OK) {
        winner = 2;
      }

      // update scores and tell the score manager
      xSemaphoreTake(scoreMutex, portMAX_DELAY);
      if (winner == 1) {
        player1_score++;
      } else if (winner == 2) {
        player2_score++;
      }

      size_t score1 = player1_score;
      size_t score2 = player2_score;
      xSemaphoreGive(scoreMutex);
      xTaskNotifyGive(score_manager_handle);

      // log the round
      char t1[24], t2[24], b1[24], b2[24];
      format_reaction(t1, sizeof t1, p1_time, r1);
      format_reaction(t2, sizeof t2, p2_time, r2);
      format_reaction(b1, sizeof b1, best_time[0], best_time[0] == NO_REACTION ? REACTION_NONE : REACTION_OK);
      format_reaction(b2, sizeof b2, best_time[1], best_time[1] == NO_REACTION ? REACTION_NONE : REACTION_OK);
      uart_printf("Round %lu (limit %lu ms) %s: P1 %s | P2 %s | %s | score %u-%u | best P1 %s, P2 %s\r\n",
                  (unsigned long)round, (unsigned long)round_timeout_ms, state_name(gameState),
                  t1, t2,
                  winner == 1 ? "P1 scores" : (winner == 2 ? "P2 scores" : "nobody"),
                  (unsigned)score1, (unsigned)score2, b1, b2);

      // if winner threshold reached, enter winstate
      if (score1 >= SCORE_THRESHOLD) {
        game_winner = 1;
      } else if (score2 >= SCORE_THRESHOLD) {
        game_winner = 2;
      }
    }

    // Win state
    set_all_leds(GPIO_PIN_RESET);
    uart_printf("*** Player %d wins! ***\r\n", game_winner);
    
    // Victory tone
    static const uint32_t notes[] = { 523, 659, 784, 1047, 784, 1047 };

    // flash LEDs
    for (uint32_t i = 0; i < sizeof(notes) / sizeof(notes[0]); i++) {
      GPIO_PinState pinState = (i % 2 == 0) ? GPIO_PIN_SET : GPIO_PIN_RESET;

      set_all_leds(pinState);
      buzz(notes[i], 150);
    }

    set_all_leds(GPIO_PIN_RESET);
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

    // Discard irrelevant inputs outside of this round.
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
    set_all_leds(GPIO_PIN_RESET);


    // determine winner. 1 if p1 wins, 2 if p2 wins.
    // a draw is assumed to be impossible
    // uint32_t winner = 1 + (player2_value  < player1_value);

    buzz_once();
    
    // send results back to round manager.
    Time_Result result = { player1_value, player2_value };
    xQueueSend(io_manager_result, &result, portMAX_DELAY);
  }
}

void player(void *args)
{

  Player_Args* player_args = (Player_Args*) args;

  QueueHandle_t inputQueue = player_args->inputQueue;
  QueueHandle_t resultQueue = player_args->resultQueue;
  State receivedState;

  for (;;) {
    // Wait until IO manager notifies start
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Timing
    uint32_t start_time = HAL_GetTick();
    uint32_t max_time = round_timeout_ms + LATE_GRACE_MS;
    uint32_t elapsed = 0;
    uint32_t reaction = NO_REACTION;
    
    // wait for input
    while(elapsed < max_time) {
      // wait for ISR message until timeout.
      if (xQueueReceive(inputQueue, &receivedState, pdMS_TO_TICKS(max_time - elapsed)) == pdPASS) {
        // Get game state mutex to verify player input
        if (xSemaphoreTake(gameStateMutex, portMAX_DELAY) == pdTRUE) {
          // correct input
          if (receivedState == gameState) { 
            reaction = HAL_GetTick() - start_time;
            xSemaphoreGive(gameStateMutex); // release mutex
            break; // exit while loop and go back to initial waiting state again
          }
          // Release mutex, go back to waiting for input
          xSemaphoreGive(gameStateMutex);
        }
      }
      elapsed = HAL_GetTick() - start_time;
    }
    xQueueOverwrite(resultQueue, &reaction);
  }
}

void score_manager(void *args) 
{
  for (;;) {
    // wait until notification is received
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // read score mutex
    xSemaphoreTake(scoreMutex, portMAX_DELAY);
    size_t score1 = player1_score;
    size_t score2 = player2_score;
    xSemaphoreGive(scoreMutex);

    // update 7 seg displays
    display_show(score1, score2);
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

  // buzzer: PWM running with the compare value 0 (silent); buzzer_tone() sets the pitch
  HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1);
  buzzer_tone(0);

  // displays: 0 - 0
  display_show(0, 0);

  gameStateMutex = xSemaphoreCreateMutex();
  scoreMutex = xSemaphoreCreateMutex();

  // Set up queues
  player1_input = xQueueCreate(8, sizeof(State));
  player2_input = xQueueCreate(8, sizeof(State));
  
  player1_result = xQueueCreate(1, sizeof(uint32_t));
  player2_result = xQueueCreate(1, sizeof(uint32_t));
  
  io_manager_result = xQueueCreate(1, sizeof(Time_Result));

  // Initialization safety check.
  if (gameStateMutex == NULL || scoreMutex == NULL || player1_input == NULL || player2_input == NULL ||
      player1_result == NULL || player2_result == NULL || io_manager_result == NULL) {
    Error_Handler();
  }

  player1_args  = {
    .inputQueue = player1_input,
    .resultQueue = player1_result
  };

  player2_args  = {
    .inputQueue = player2_input,
    .resultQueue = player2_result
  };

  // Create all tasks with error handling.
  if (xTaskCreate(io_manager, "io_manager", STACK_WORDS, NULL, PRIO_IO, &io_manager_handle) != pdPASS ||
      xTaskCreate(player, "player1", STACK_WORDS, &player1_args, PRIO_PLAYER, &player1_handle) != pdPASS ||
      xTaskCreate(player, "player2", STACK_WORDS, &player2_args, PRIO_PLAYER, &player2_handle) != pdPASS ||
      xTaskCreate(score_manager, "score_manager", STACK_WORDS, NULL, PRIO_SCORE, &score_manager_handle) != pdPASS ||
      xTaskCreate(round_manager, "round_manager", STACK_ROUND_WORDS, NULL, PRIO_ROUND, &round_manager_handle) != pdPASS ||
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
    } else if (GPIO_Pin == pin_pull1) {
        queue = player1_input;
        state = PULL;
    // Player 2
    } else if (GPIO_Pin == pin_bop2) {
        queue = player2_input;
        state = BOP;
    } else if (GPIO_Pin == pin_pull2) {
        queue = player2_input;
        state = PULL;
    } else if (GPIO_Pin == B1_Pin) {
      // Restart the game

      static uint32_t last_b1_ms;
      uint32_t now_b1 = HAL_GetTick();
      if ((now_b1 - last_b1_ms) >= DEBOUNCE_MS) {
          last_b1_ms = now_b1;
          if (round_manager_handle != NULL)
            vTaskNotifyGiveFromISR(round_manager_handle, &higher_priority_task_woken);
      }
      portYIELD_FROM_ISR(higher_priority_task_woken);
      return; 
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
