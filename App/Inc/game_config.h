/* game_config.h - every tunable number of the game in one place. */

#ifndef GAME_CONFIG_H
#define GAME_CONFIG_H

/* ---- Rules ---------------------------------------------------------- */
#define WIN_SCORE                 5U      /* first to this many points wins (max 9, one digit) */

/* Time a player has to react after the cue. It shrinks every round. */
#define ROUND_TIMEOUT_START_MS    3000U
#define ROUND_TIMEOUT_STEP_MS     100U    /* shorter by this much each round */
#define ROUND_TIMEOUT_MIN_MS      1000U

/* Random wait between the start beep and the cue LED (no anticipating) */
#define CUE_DELAY_MIN_MS          500U
#define CUE_DELAY_MAX_MS          2000U

/* After the first player reacts, wait this long for the other one. Their
 * microsecond timestamps then decide who was first. */
#define NEAR_TIE_WINDOW_MS        10U

/* Potentiometer: counts (of 4095) it must move from where it was at the cue */
#define POT_MOVE_THRESHOLD        1000

/* ---- Pauses and feedback -------------------------------------------- */
#define START_PAUSE_MS            500U    /* after B1 is pressed */
#define ROUND_PAUSE_MS            800U    /* between rounds */
#define IDLE_WAIT_MS              200U    /* max wait for player tasks to go idle */

#define BEEP_START_HZ             1000U
#define BEEP_START_MS             150U
#define BEEP_POINT_HZ             1500U
#define BEEP_POINT_MS             120U
#define BEEP_NOBODY_HZ            300U
#define BEEP_NOBODY_MS            300U

#endif /* GAME_CONFIG_H */
