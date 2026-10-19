/* score.c - Score manager.
 *
 * The scores are shared data, so every access takes scoreMutex. Whoever
 * changes a score only sets a thread flag; the score task then reads the
 * scores and sends them to the displays. Nobody else touches the displays. */

#include "score.h"
#include "main.h"

#define SCORE_MAX           9U            /* one digit per player */
#define SCORE_FLAG_UPDATE   0x00000001U
#define FLAGS_ERROR_BIT     0x80000000U   /* set in osThreadFlags* error codes */

static uint8_t       scores[PLAYER_COUNT];
static osMutexId_t   scoreMutex  = NULL;
static osThreadId_t  scoreThread = NULL;

static void prvScoreTask(void *argument)
{
    (void)argument;

    for (;;) {
        /* 1. wait for a notification that the scores changed */
        uint32_t flags = osThreadFlagsWait(SCORE_FLAG_UPDATE, osFlagsWaitAny, osWaitForever);
        if ((flags & FLAGS_ERROR_BIT) != 0U) {
            continue;
        }

        /* 2. read both scores in one go, 3. send them to the displays */
        osMutexAcquire(scoreMutex, osWaitForever);
        uint8_t p1 = scores[PLAYER_1];
        uint8_t p2 = scores[PLAYER_2];
        osMutexRelease(scoreMutex);

        Board_DisplayScores(p1, p2);
    }
}

void Score_Init(void)
{
    scoreMutex = osMutexNew(NULL);
    if (scoreMutex == NULL) {
        Error_Handler();
    }

    const osThreadAttr_t attr = {
        .name = "Score", .stack_size = 512U, .priority = osPriorityBelowNormal };
    scoreThread = osThreadNew(prvScoreTask, NULL, &attr);
    if (scoreThread == NULL) {
        Error_Handler();
    }
}

void Score_Reset(void)
{
    osMutexAcquire(scoreMutex, osWaitForever);
    for (int p = 0; p < PLAYER_COUNT; p++) {
        scores[p] = 0U;
    }
    osMutexRelease(scoreMutex);
    (void)osThreadFlagsSet(scoreThread, SCORE_FLAG_UPDATE);
}

void Score_Add(Player_t player)
{
    osMutexAcquire(scoreMutex, osWaitForever);
    if (scores[player] < SCORE_MAX) {
        scores[player]++;
    }
    osMutexRelease(scoreMutex);
    (void)osThreadFlagsSet(scoreThread, SCORE_FLAG_UPDATE);
}

uint8_t Score_Get(Player_t player)
{
    osMutexAcquire(scoreMutex, osWaitForever);
    uint8_t s = scores[player];
    osMutexRelease(scoreMutex);
    return s;
}
