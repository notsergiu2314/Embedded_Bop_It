/* board.h - the only place that knows which Nucleo pin does what.
 * The game logic uses Player_t / InputType_t and never touches a GPIO.
 * Pin map: docs/PINOUT.md */

#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "cmsis_os.h"

typedef enum { PLAYER_1 = 0, PLAYER_2, PLAYER_COUNT } Player_t;
typedef enum { INPUT_BUTTON = 0, INPUT_SWITCH, INPUT_POT, INPUT_COUNT } InputType_t;

/* One press / flip. Created in the EXTI interrupt and put in the queue of
 * the player it belongs to. */
typedef struct {
    Player_t    player;
    InputType_t input;          /* INPUT_BUTTON or INPUT_SWITCH */
    uint32_t    timestamp_us;   /* TIM2, 1 MHz, wraps every ~71 min */
} InputEvent_t;

/* Call once after osKernelInitialize().
 * Every button/switch edge of a player is put into that player's queue
 * (element: InputEvent_t), so each player task only sees its own events. */
void Board_Init(const osMessageQueueId_t inputQueues[PLAYER_COUNT]);

/* Blue button B1 (start / restart). Waits until it is pressed, true if it was
 * pressed before the timeout (in kernel ticks). A timeout of 0 only checks, so
 * it can also be used to throw away an old press. */
bool Board_WaitStartButton(uint32_t timeout_ticks);

/* Microseconds from the free-running timer. Use it for every timestamp. */
uint32_t Board_NowUs(void);

/* Cue LEDs: one per input, active high. */
void Board_LedSet(Player_t player, InputType_t input, bool on);
void Board_LedsAllOff(void);

/* Raw potentiometer value, 0..4095 (12 bit). */
uint16_t Board_PotRead(Player_t player);

/* Scores 0..9 on the two 7-segment displays. */
void Board_DisplayScores(uint8_t scoreP1, uint8_t scoreP2);

/* Passive buzzer: square wave at freq_hz, 0 = silence. */
void Board_BuzzerTone(uint32_t freq_hz);

#endif /* BOARD_H */
