/* game.c - the reaction game: Round manager, I/O manager, Player 1/2 tasks.
 *
 *   Round manager --request--> I/O manager --command--> Player 1 task
 *        ^                         |   ^                 Player 2 task
 *        +------- result ----------+   +---- result + "done" flag ----+
 *
 * Round manager: rounds, win check, buzzer, start beep, score updates.
 * I/O manager:   picks the random input, lights the LEDs, starts both
 *                player tasks, judges who was first, shows the result.
 * Player task:   waits for ITS player to use the requested input, ignores
 *                every other input, reports the timestamp.
 *
 * All timestamps are microseconds from Board_NowUs(). Button and switch
 * timestamps are taken in the interrupt; the pot is polled every ~1 ms. */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "game.h"
#include "game_config.h"
#include "board.h"
#include "score.h"
#include "main.h"

extern UART_HandleTypeDef huart2;

_Static_assert(WIN_SCORE >= 1U && WIN_SCORE <= 9U, "WIN_SCORE must fit one digit");

/* ------------------------------------------------------------------ */
/* Messages and flags                                                 */
/* ------------------------------------------------------------------ */
#define WINNER_NONE         (-1)

/* Round manager -> I/O manager */
typedef struct {
    uint32_t round;
    uint32_t timeout_ms;
} RoundRequest_t;

/* I/O manager -> Round manager */
typedef struct {
    int         winner;                       /* Player_t, or WINNER_NONE */
    InputType_t target;
    bool        responded[PLAYER_COUNT];
    uint32_t    reaction_us[PLAYER_COUNT];    /* valid if responded */
} RoundResult_t;

/* I/O manager -> player task */
typedef struct {
    InputType_t target;
    uint32_t    cue_us;     /* when the LED was lit */
} PlayerCmd_t;

/* player task -> I/O manager */
typedef struct {
    Player_t player;
    uint32_t timestamp_us;  /* when the correct input happened */
} PlayerResult_t;

/* thread flags */
#define PLAYER_FLAG_ABORT   0x00000001U                  /* to a player task */
#define IO_FLAG_DONE(p)     (1UL << (unsigned)(p))       /* to the I/O manager */
#define IO_FLAGS_ALL        ((1UL << PLAYER_COUNT) - 1UL)
#define FLAGS_ERROR_BIT     0x80000000U                  /* set in osThreadFlags* errors */

/* ------------------------------------------------------------------ */
/* Objects                                                            */
/* ------------------------------------------------------------------ */
static osMessageQueueId_t roundReqQueue    = NULL;   /* Round mgr -> I/O mgr */
static osMessageQueueId_t roundResultQueue = NULL;   /* I/O mgr -> Round mgr */
static osMessageQueueId_t playerInputQueue[PLAYER_COUNT];   /* ISR -> player task */
static osMessageQueueId_t playerCmdQueue[PLAYER_COUNT];     /* I/O mgr -> player task */
static osMessageQueueId_t playerResultQueue = NULL;         /* player tasks -> I/O mgr */
static osThreadId_t       ioThread          = NULL;
static osThreadId_t       playerThread[PLAYER_COUNT];

static const char *const targetName[INPUT_COUNT] = { "BUTTON", "SWITCH", "POT" };

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */
static uint32_t prvMsToTicks(uint32_t ms)
{
    uint32_t ticks = (uint32_t)(((uint64_t)ms * osKernelGetTickFreq()) / 1000U);
    return (ticks == 0U) ? 1U : ticks;
}

/* xorshift32, only used by the I/O manager */
static uint32_t prvRandom(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Only the Round manager prints, so the UART needs no lock. */
static void prvPrint(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        HAL_UART_Transmit(&huart2, (uint8_t *)buf,
                          (uint16_t)((n < (int)sizeof buf) ? n : (int)sizeof buf - 1),
                          100);
    }
}

/* ------------------------------------------------------------------ */
/* Player tasks (one function, two instances)                         */
/* ------------------------------------------------------------------ */
static void prvPlayerTask(void *argument)
{
    const Player_t me = (Player_t)(uintptr_t)argument;

    for (;;) {
        /* Idle: wait for the I/O manager to name the input to wait for */
        PlayerCmd_t cmd;
        if (osMessageQueueGet(playerCmdQueue[me], &cmd, NULL, osWaitForever) != osOK) {
            continue;
        }

        (void)osThreadFlagsClear(PLAYER_FLAG_ABORT);       /* forget an old abort */
        const int32_t pot_start = (int32_t)Board_PotRead(me);
        bool responded = false;
        PlayerResult_t res = { me, 0U };

        while (!responded) {
            /* Button / switch: the interrupt already took the timestamp.
             * The 1 tick timeout is also the 1 ms pot polling period. */
            InputEvent_t e;
            if (osMessageQueueGet(playerInputQueue[me], &e, NULL, 1U) == osOK) {
                /* Wrong input: ignored. Event older than the cue: ignored. */
                if ((e.input == cmd.target) && ((int32_t)(e.timestamp_us - cmd.cue_us) >= 0)) {
                    res.timestamp_us = e.timestamp_us;
                    responded = true;
                }
            }

            /* Pot: counts as the input once it moved far enough from where it
             * was at the cue (any direction), so it works from any position. */
            if (!responded && (cmd.target == INPUT_POT)) {
                int32_t delta = (int32_t)Board_PotRead(me) - pot_start;
                if (delta < 0) {
                    delta = -delta;
                }
                if (delta >= POT_MOVE_THRESHOLD) {
                    res.timestamp_us = Board_NowUs();
                    responded = true;
                }
            }

            /* The I/O manager ends the round (other player won, or timeout) */
            if (!responded && ((osThreadFlagsGet() & PLAYER_FLAG_ABORT) != 0U)) {
                break;
            }
        }

        if (responded) {
            (void)osMessageQueuePut(playerResultQueue, &res, 0U, 0U);
        }
        (void)osThreadFlagsSet(ioThread, IO_FLAG_DONE(me));   /* back to idle */
    }
}

/* ------------------------------------------------------------------ */
/* I/O manager                                                        */
/* ------------------------------------------------------------------ */
static void prvRecord(RoundResult_t *out, uint32_t stamp_us[], uint32_t cue_us,
                      const PlayerResult_t *r)
{
    if (!out->responded[r->player]) {
        out->responded[r->player]   = true;
        out->reaction_us[r->player] = r->timestamp_us - cue_us;
        stamp_us[r->player]         = r->timestamp_us;
    }
}

static void prvIoManagerTask(void *argument)
{
    (void)argument;
    uint32_t rng = 0U;

    for (;;) {
        RoundRequest_t req;
        if (osMessageQueueGet(roundReqQueue, &req, NULL, osWaitForever) != osOK) {
            continue;
        }

        /* 0. Random input, the same for both players. Seeded from the timer
         * on the first round (the start press is human timing). A random
         * wait first, so the cue cannot be anticipated. */
        if (rng == 0U) {
            rng = Board_NowUs() | 1U;
        }
        const InputType_t target = (InputType_t)(prvRandom(&rng) % INPUT_COUNT);
        osDelay(prvMsToTicks(CUE_DELAY_MIN_MS +
                             prvRandom(&rng) % (CUE_DELAY_MAX_MS - CUE_DELAY_MIN_MS + 1U)));

        /* Forget everything that happened before the cue (early presses) */
        for (int p = 0; p < PLAYER_COUNT; p++) {
            (void)osMessageQueueReset(playerInputQueue[p]);
        }
        (void)osMessageQueueReset(playerResultQueue);
        (void)osThreadFlagsClear(IO_FLAGS_ALL);

        /* 1. Light the LED of that input for both players, note the time */
        for (int p = 0; p < PLAYER_COUNT; p++) {
            Board_LedSet((Player_t)p, target, true);
        }
        const uint32_t cue_us = Board_NowUs();

        /* 2. Start both player tasks */
        const PlayerCmd_t cmd = { target, cue_us };
        for (int p = 0; p < PLAYER_COUNT; p++) {
            (void)osMessageQueuePut(playerCmdQueue[p], &cmd, 0U, 0U);
        }

        /* 3. Wait for the first one. Then give the other player a very short
         * time: both may have reacted almost together and the timestamps
         * decide who was first, not the order the tasks happened to run. */
        RoundResult_t out = { WINNER_NONE, target, { false, false }, { 0U, 0U } };
        uint32_t stamp_us[PLAYER_COUNT] = { 0U, 0U };
        PlayerResult_t r;

        if (osMessageQueueGet(playerResultQueue, &r, NULL, prvMsToTicks(req.timeout_ms)) == osOK) {
            prvRecord(&out, stamp_us, cue_us, &r);
            if (osMessageQueueGet(playerResultQueue, &r, NULL,
                                  prvMsToTicks(NEAR_TIE_WINDOW_MS)) == osOK) {
                prvRecord(&out, stamp_us, cue_us, &r);
            }
        }

        /* 4. Make sure both player tasks are idle again */
        for (int p = 0; p < PLAYER_COUNT; p++) {
            (void)osThreadFlagsSet(playerThread[p], PLAYER_FLAG_ABORT);
        }
        (void)osThreadFlagsWait(IO_FLAGS_ALL, osFlagsWaitAll, prvMsToTicks(IDLE_WAIT_MS));
        while (osMessageQueueGet(playerResultQueue, &r, NULL, 0U) == osOK) {
            prvRecord(&out, stamp_us, cue_us, &r);      /* a result that came in late */
        }

        /* 5. Winner: the earlier timestamp. Exactly equal = nobody. */
        if (out.responded[PLAYER_1] && out.responded[PLAYER_2]) {
            int32_t d = (int32_t)(stamp_us[PLAYER_2] - stamp_us[PLAYER_1]);
            out.winner = (d > 0) ? PLAYER_1 : ((d < 0) ? PLAYER_2 : WINNER_NONE);
        } else if (out.responded[PLAYER_1]) {
            out.winner = PLAYER_1;
        } else if (out.responded[PLAYER_2]) {
            out.winner = PLAYER_2;
        }

        /* 6. Show the result: LEDs off, winner's LEDs flash */
        Board_LedsAllOff();
        if (out.winner != WINNER_NONE) {
            for (int i = 0; i < 3; i++) {
                for (int in = 0; in < INPUT_COUNT; in++) {
                    Board_LedSet((Player_t)out.winner, (InputType_t)in, true);
                }
                osDelay(prvMsToTicks(100U));
                Board_LedsAllOff();
                osDelay(prvMsToTicks(100U));
            }
        }

        /* 7. Tell the Round manager */
        (void)osMessageQueuePut(roundResultQueue, &out, 0U, osWaitForever);
    }
}

/* ------------------------------------------------------------------ */
/* Round manager                                                      */
/* ------------------------------------------------------------------ */
static void prvBeep(uint32_t freq_hz, uint32_t ms)
{
    Board_BuzzerTone(freq_hz);
    osDelay(prvMsToTicks(ms));
    Board_BuzzerTone(0U);
}

/* Reaction time as text: "312.4 ms", or "--" if the player did not react */
static void prvFormatReaction(char *buf, size_t len, bool responded, uint32_t us)
{
    if (responded) {
        (void)snprintf(buf, len, "%lu.%lu ms", (unsigned long)(us / 1000U),
                       (unsigned long)((us % 1000U) / 100U));
    } else {
        (void)snprintf(buf, len, "--");
    }
}

/* The response time shrinks every round, down to a minimum */
static uint32_t prvTimeoutMs(uint32_t round)
{
    const uint32_t cut = (round - 1U) * ROUND_TIMEOUT_STEP_MS;
    const uint32_t max_cut = ROUND_TIMEOUT_START_MS - ROUND_TIMEOUT_MIN_MS;
    return (cut >= max_cut) ? ROUND_TIMEOUT_MIN_MS : (ROUND_TIMEOUT_START_MS - cut);
}

/* Winner fanfare: rising notes while the winner's LEDs blink */
static void prvGameOverShow(Player_t winner)
{
    static const uint32_t notes[] = { 523U, 659U, 784U, 1047U, 784U, 1047U };
    for (unsigned i = 0; i < sizeof notes / sizeof notes[0]; i++) {
        for (int in = 0; in < INPUT_COUNT; in++) {
            Board_LedSet(winner, (InputType_t)in, (i % 2U) == 0U);
        }
        prvBeep(notes[i], 150U);
    }
    Board_LedsAllOff();
}

static void prvRoundManagerTask(void *argument)
{
    (void)argument;

    for (;;) {
        /* Wait for the blue button (an old press is thrown away first) */
        prvPrint("\r\n=== Bop-It === press the blue button to start (first to %u wins)\r\n",
                 (unsigned)WIN_SCORE);
        (void)Board_WaitStartButton(0U);
        (void)Board_WaitStartButton(osWaitForever);

        Score_Reset();
        osDelay(prvMsToTicks(START_PAUSE_MS));

        int game_winner = WINNER_NONE;
        for (uint32_t round = 1U; game_winner == WINNER_NONE; round++) {
            const uint32_t timeout_ms = prvTimeoutMs(round);

            prvBeep(BEEP_START_HZ, BEEP_START_MS);              /* "round starts" */

            /* Hand the round to the I/O manager and wait for its result */
            const RoundRequest_t req = { round, timeout_ms };
            (void)osMessageQueuePut(roundReqQueue, &req, 0U, osWaitForever);
            RoundResult_t res;
            while (osMessageQueueGet(roundResultQueue, &res, NULL, osWaitForever) != osOK) {
            }

            /* Point to the winner, feedback */
            if (res.winner != WINNER_NONE) {
                Score_Add((Player_t)res.winner);
                prvBeep(BEEP_POINT_HZ, BEEP_POINT_MS);
            } else {
                prvBeep(BEEP_NOBODY_HZ, BEEP_NOBODY_MS);
            }

            char r1[16], r2[16];
            prvFormatReaction(r1, sizeof r1, res.responded[PLAYER_1], res.reaction_us[PLAYER_1]);
            prvFormatReaction(r2, sizeof r2, res.responded[PLAYER_2], res.reaction_us[PLAYER_2]);
            prvPrint("Round %lu (%lu ms): %s -> %s | P1 %s, P2 %s | score %u-%u\r\n",
                     (unsigned long)round, (unsigned long)timeout_ms, targetName[res.target],
                     (res.winner == WINNER_NONE) ? "nobody"
                         : ((res.winner == PLAYER_1) ? "P1" : "P2"),
                     r1, r2, (unsigned)Score_Get(PLAYER_1), (unsigned)Score_Get(PLAYER_2));

            /* Has anyone reached the winning score? */
            for (int p = 0; p < PLAYER_COUNT; p++) {
                if (Score_Get((Player_t)p) >= WIN_SCORE) {
                    game_winner = p;
                }
            }
            if (game_winner == WINNER_NONE) {
                osDelay(prvMsToTicks(ROUND_PAUSE_MS));
            }
        }

        prvPrint("*** Player %d wins! ***\r\n", game_winner + 1);
        prvGameOverShow((Player_t)game_winner);
    }
}

/* ------------------------------------------------------------------ */
/* Setup                                                              */
/* ------------------------------------------------------------------ */
void Game_Init(void)
{
    /* Queues. The element size is the full structure. */
    roundReqQueue    = osMessageQueueNew(1U, sizeof(RoundRequest_t), NULL);
    roundResultQueue = osMessageQueueNew(1U, sizeof(RoundResult_t), NULL);
    playerResultQueue = osMessageQueueNew(PLAYER_COUNT, sizeof(PlayerResult_t), NULL);
    for (int p = 0; p < PLAYER_COUNT; p++) {
        playerInputQueue[p] = osMessageQueueNew(8U, sizeof(InputEvent_t), NULL);
        playerCmdQueue[p]   = osMessageQueueNew(1U, sizeof(PlayerCmd_t), NULL);
    }
    if ((roundReqQueue == NULL) || (roundResultQueue == NULL) || (playerResultQueue == NULL)) {
        Error_Handler();
    }
    for (int p = 0; p < PLAYER_COUNT; p++) {
        if ((playerInputQueue[p] == NULL) || (playerCmdQueue[p] == NULL)) {
            Error_Handler();
        }
    }

    Board_Init(playerInputQueue);
    Score_Init();

    /* Tasks. Players and I/O manager above the others: they time the game. */
    const osThreadAttr_t ioAttr = {
        .name = "IoMgr", .stack_size = 768U, .priority = osPriorityAboveNormal };
    const osThreadAttr_t p1Attr = {
        .name = "Player1", .stack_size = 512U, .priority = osPriorityAboveNormal };
    const osThreadAttr_t p2Attr = {
        .name = "Player2", .stack_size = 512U, .priority = osPriorityAboveNormal };
    const osThreadAttr_t roundAttr = {   /* needs room for snprintf */
        .name = "Round", .stack_size = 2048U, .priority = osPriorityNormal };

    ioThread = osThreadNew(prvIoManagerTask, NULL, &ioAttr);
    playerThread[PLAYER_1] = osThreadNew(prvPlayerTask, (void *)(uintptr_t)PLAYER_1, &p1Attr);
    playerThread[PLAYER_2] = osThreadNew(prvPlayerTask, (void *)(uintptr_t)PLAYER_2, &p2Attr);
    if ((ioThread == NULL) || (playerThread[PLAYER_1] == NULL) || (playerThread[PLAYER_2] == NULL)) {
        Error_Handler();
    }
    if (osThreadNew(prvRoundManagerTask, NULL, &roundAttr) == NULL) {
        Error_Handler();
    }
}
