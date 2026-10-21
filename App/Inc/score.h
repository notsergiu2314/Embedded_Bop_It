/* score.h - score manager: owns the scores and the 7-segment displays. */

#ifndef SCORE_H
#define SCORE_H

#include <stdint.h>

#include "board.h"

/* Creates the mutex and the score task. Call after Board_Init(). */
void Score_Init(void);

/* Set both scores to 0 (the displays follow). */
void Score_Reset(void);

/* Give a player one point (capped at 9). The displays follow. */
void Score_Add(Player_t player);

uint8_t Score_Get(Player_t player);

#endif /* SCORE_H */
