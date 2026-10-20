# TODO and mapping: `main.c` skeleton -> `App/` code

The skeleton in `Core/Src/main.c` (lines 55-163) says *what* the game does. The code in `App/`
already contains one working-on-paper implementation of every step (it builds, but has **not** been
run on hardware). Use it as a reference, copy from it, or replace it. Line numbers refer to the
current commit and will drift.

Legend: **done** = exists in `App/`, **partly** = exists but differs from the skeleton, **missing** = nobody has it yet.

## 1. Status of `main.c`

`main.c` is the product. `App/` is **excluded from the build** (removed from the CubeIDE source folders in
`.cproject`) and only stays in the repository as a reference. We do not regenerate from CubeMX for now, so
code outside the `USER CODE` blocks is tolerated.

**It builds and links.** The compile errors of the first versions were fixed (see "What was a typo" below).
Choices made for now: a round ends when **both** players have reacted; if nobody reacts nothing happens;
`SCORE_THRESHOLD` is **9** (one display digit, change it in `main.c`); a tie goes to player 2 (see below).

### Done on `sergiu-test` (not tested on the board yet)

| # | What | How |
|---|---|---|
| 1 | Tasks, queues and mutex are created | `game_init()` in `main.c`, called from `USER CODE BEGIN 2` (before the scheduler starts). Player tasks get a `Player_Args *`. Task priorities and stack size are the `PRIO_*` / `STACK_WORDS` defines. |
| 2 | Real pins | LEDs: `led_bop` D9 (PC7), `led_pull` D15 (PB8), `led_twist` D14 (PB9), through `led_write()`. Inputs: `pin_bop1/2` = PA10 / PA8, `pin_pull1/2` = PB4 / PA9. `pin_twist1/2` are 0 (the pot has no interrupt pin). |
| 3 | Random | `random_next()` keeps its state (xorshift32, seeded once from the tick + SysTick). `get_random_gameState()` picks from the `playable` list. The seed is only the start-up time, so the sequence can repeat after a reset. |
| 4 | One signal between the managers | The round manager's extra `ulTaskNotifyTake` is gone. The I/O manager sends a `Time_Result` through `io_manager_result`. |
| 5 | **TWIST (pot)** | `game_init()` starts the ADC with DMA (`HAL_ADC_Start_DMA`, circular) and switches the DMA interrupt off. `pot_monitor` (task, every `POT_POLL_MS` = 1 ms) compares `pot_raw[]` with the position at the cue (`pot_arm()`, called by the I/O manager) and, when a pot moved `TWIST_THRESHOLD` (1000 of 4095) in either direction, puts a `TWIST` message in that player's input queue, like the EXTI callback does for buttons. The player tasks are unchanged. Accuracy about 1 ms. |
| 6 | **Debounce and stale input** | In `HAL_GPIO_EXTI_Callback`: after an accepted edge, further edges on the same pin are ignored for `DEBOUNCE_MS` (30 ms). The I/O manager calls `xQueueReset` on both player input queues right before the cue, so early presses and leftover bounces do not count. |

Extras needed to make it usable (change or remove them freely):
- `TWIST_ENABLED` is **1**. Set it to 0 to ask only BOP and PULL (e.g. to test without the pots).
- `ROUND_PAUSE_MS` (1000): pause before each round, otherwise the next LED lights the instant the previous round ends.
- `score_manager` blocks on `ulTaskNotifyTake` (its first TODO line); an empty loop would have used all the CPU.

### Still missing

| # | What is missing | Where | Note |
|---|---|---|---|
| 7 | Score manager body, displays, buzzer, winstate, winner indication, and storing the timings are still TODO. | `score_manager()`, `buzz_once()` | [score.c](../App/Src/score.c), `Board_DisplayScores` ([board.c:141](../App/Src/board.c#L141)), `Board_BuzzerTone` ([board.c:157](../App/Src/board.c#L157)). The round manager also never wakes the score manager yet. |
| 8 | Nothing is printed, so the only feedback is the cue LED. | | a UART line per round (reaction times, score) would help testing |

### Known behaviour we accepted for now
- **Tie** (both players measure the same time in ms): `if (p1_time < p2_time)` is false, so **player 2 gets the point**. With 1 ms resolution this can happen in roughly 1 round out of 70 when two players react about equally fast (rough estimate, not measured).
- **Nobody reacts / one player never reacts:** the I/O manager waits forever for both results, so the round never ends.
- Times are measured in the player tasks with `HAL_GetTick()` (1 ms), starting when the task wakes up. Player 1 is notified first, so it starts a little earlier than player 2.

### What was a typo and what is a real FreeRTOS name
| Written | Was | Now |
|---|---|---|
| `ulTaskNotifyTake(...)` | **real**: the *receiver* waits for a notification | unchanged |
| `xTaskNotifyGive(task)` | **real**: the *sender* gives a notification | unchanged |
| `ulTaskNotifyGive(task)` | typo, this name does not exist | `xTaskNotifyGive(task);` |
| `portMAX_Delay` | typo (case) | `portMAX_DELAY` |
| `xQueueOverwrite(q, end_time)` | wrong call: the 2nd argument is a **pointer** to the item (and the queue length must be 1) | `xQueueOverwrite(q, &end_time);` |
| `p1_time`, `end_time`, `player1_value`, ... | used without a type | declared as `uint32_t` |
| `args.inputQueue` | `args` is a `void *`, it has no fields | `Player_Args` struct, read through a cast |
| stray `State` line in `main()`, backtick, `schedStateuler` | accidental typing | removed / restored |
| `true` | needs `<stdbool.h>` | included in `USER CODE BEGIN Includes` |
| missing `;` | in about ten places | added |

Note: `xTaskNotifyGive` / `ulTaskNotifyTake` pair up like a semaphore: Give increments a counter in the target task,
Take (with `pdTRUE`) waits until it is non-zero and clears it.

## 2. Mapping: skeleton -> `App/` code

*`App/` is not built any more. The rows below point to it as a reference implementation; the "skeleton" column is the original outline, `main.c` has since been filled in (see section 1).*

Names: **BOP** = push button = `INPUT_BUTTON`, **PULL** = slide switch = `INPUT_SWITCH`,
**TWIST** = potentiometer = `INPUT_POT` ([board.h:14](../App/Inc/board.h#L14)).

### Declarations (main.c 55-92)

| Skeleton | Status | `App/` equivalent |
|---|---|---|
| task handles `round_manager_handle`, `io_manager_handle`, `player1/2_handle`, `score_manager_handle` | partly | `ioThread` and `playerThread[]` in [game.c:76-77](../App/Src/game.c#L76), `scoreThread` in [score.c:16](../App/Src/score.c#L16). The round manager needs no handle, nobody signals it directly (it receives through a queue). |
| `enum state { BOP, TWIST, PULL }` | done | `InputType_t` ([board.h:14](../App/Inc/board.h#L14)). Names for printing: `targetName[]` ([game.c:79](../App/Src/game.c#L79)). |
| `led_twist`, `led_bop`, `led_pull` (one LED each) | done | Matches the hardware now: 3 cue LEDs, each one linked to the other player's matching LED (driven by the `_P1` pin; the `_P2` pins are spare and `App/` sets both together). Table `led_pin[player][input]` ([board.c:28](../App/Src/board.c#L28)), setter `Board_LedSet()` ([board.c:109](../App/Src/board.c#L109)). |
| `pin_twist1/2`, `pin_bop1/2`, `pin_pull1/2` | done | `exti_input[]` ([board.c:45](../App/Src/board.c#L45)) for button and switch, `Board_PotRead()` ([board.c:124](../App/Src/board.c#L124)) for the pot. |
| `pin_score` (7 seg) | partly | There is no score pin: the two 595s are driven over SPI + a latch pin. `Board_DisplayScores()` ([board.c:141](../App/Src/board.c#L141)). |
| `pin_buzzer` | partly | The buzzer is **not** a GPIO: it is TIM4 PWM on PB6. `Board_BuzzerTone()` ([board.c:157](../App/Src/board.c#L157)). |

### `buzz_once()` (main.c 96)

| Skeleton | Status | `App/` equivalent |
|---|---|---|
| `//TODO: buzzer implementation` | done | `prvBeep(freq, ms)` ([game.c:281](../App/Src/game.c#L281)): tone on, `osDelay`, tone off. Beep constants in [game_config.h](../App/Inc/game_config.h). |

### `round_manager()` (main.c 101-122)

| Skeleton step | Status | `App/` equivalent |
|---|---|---|
| initialize registers, pins, variables | done | `Game_Init()` ([game.c:383](../App/Src/game.c#L383)) creates queues and tasks; `Board_Init()` ([board.c:70](../App/Src/board.c#L70)) starts TIM2 (timestamps), TIM4 (buzzer), ADC+DMA. Must run after `osKernelInitialize()`. |
| turn off all LEDs | done | `Board_LedsAllOff()` ([board.c:115](../App/Src/board.c#L115)) |
| reset inputs/outputs except scores | done | `osMessageQueueReset(playerInputQueue[p])` and flag clearing in the I/O manager, right before the cue ([game.c:207-212](../App/Src/game.c#L207)). Resetting *before the cue* is what stops an early press from counting. |
| buzz once | done | `prvBeep(BEEP_START_HZ, ...)` ([game.c:338](../App/Src/game.c#L338)) |
| start I/O manager | done | `osMessageQueuePut(roundReqQueue, ...)` ([game.c:342](../App/Src/game.c#L342)), carries round number and time limit |
| wait for I/O manager | done | `osMessageQueueGet(roundResultQueue, ...)` ([game.c:344](../App/Src/game.c#L344)) |
| update scores / score manager | done | `Score_Add(winner)` ([game.c:349](../App/Src/game.c#L349)) -> [score.c:64](../App/Src/score.c#L64) |
| if winner threshold reached: enter winstate | done | check at [game.c:364-369](../App/Src/game.c#L364), `prvGameOverShow()` ([game.c:308](../App/Src/game.c#L308)), then wait for B1 to restart ([game.c:328](../App/Src/game.c#L328)) |

### `io_manager()` (main.c 125-138)

| Skeleton step | Status | `App/` equivalent |
|---|---|---|
| wait for round manager notification | done | `osMessageQueueGet(roundReqQueue, ...)` at the top of `prvIoManagerTask` ([game.c:186](../App/Src/game.c#L186)) |
| (design md) determine random input | done | `prvRandom(&rng) % INPUT_COUNT` ([game.c:203](../App/Src/game.c#L203)), same input for both players. Seeded from the timer on the first round. |
| (design md) light the LEDs | done | [game.c:215-218](../App/Src/game.c#L215), and the cue timestamp `Board_NowUs()` right after |
| start player tasks 1 and 2 | done | one command per player into `playerCmdQueue[p]` ([game.c:223](../App/Src/game.c#L223)) |
| wait | done | first result with a timeout ([game.c:233](../App/Src/game.c#L233)), then a 10 ms window for the other player ([game.c:236](../App/Src/game.c#L236)); abort flag + "done" flags to make sure both are idle ([game.c:243-245](../App/Src/game.c#L243)) |
| determine winner | done | earlier **timestamp** wins, equal = nobody ([game.c:251](../App/Src/game.c#L251)) |
| winner indication (buzz, flash LEDs) | partly | LED flash in the I/O manager ([game.c:260](../App/Src/game.c#L260)); the buzz is done by the round manager ([game.c:349-353](../App/Src/game.c#L349)). Decide who owns the buzzer. |
| notify round manager | done | `osMessageQueuePut(roundResultQueue, ...)` ([game.c:274](../App/Src/game.c#L274)) |

### `playerLogic()` / `player1()` / `player2()` (main.c 140-155)

| Skeleton step | Status | `App/` equivalent |
|---|---|---|
| `playerLogic(/*TODO: Task handles*/)`, both tasks share it | done | one function `prvPlayerTask(void *argument)` ([game.c:119](../App/Src/game.c#L119)), the argument selects the player. No handles needed: it uses its player's queues. |
| (design md) wait for the correct input, ignore the others | done | [game.c:141](../App/Src/game.c#L141): the event must match the requested input **and** be newer than the cue |
| pot (TWIST) | done | polled every ~1 ms; correct once it moved `POT_MOVE_THRESHOLD` from where it was at the cue ([game.c:131, 150](../App/Src/game.c#L131)) |
| (design md) notify I/O manager (bool or timestamp) | done | timestamp into `playerResultQueue` ([game.c:167](../App/Src/game.c#L167)) + "done" flag ([game.c:169](../App/Src/game.c#L169)) |
| button / switch edges, debounce, timestamp | done | `HAL_GPIO_EXTI_Callback()` ([board.c:171](../App/Src/board.c#L171)), 30 ms debounce, put into the player's queue |

### `score_manager()` (main.c 157-163)

| Skeleton step | Status | `App/` equivalent |
|---|---|---|
| wait until notification | done | `osThreadFlagsWait` ([score.c:24](../App/Src/score.c#L24)) |
| read score mutex | done | `osMutexAcquire(scoreMutex)` ([score.c:30](../App/Src/score.c#L30)); every access goes through `Score_Add/Get/Reset` |
| update 7 seg displays | done | `Board_DisplayScores(p1, p2)` ([score.c:35](../App/Src/score.c#L35)) |

## 3. Things we might miss

### Build and CubeMX
- [ ] **Only one `HAL_GPIO_EXTI_Callback` may exist.** `board.c` defines it. If you write another one in `main.c` the link fails with a duplicate symbol. Put new interrupt code in the existing function.
- [ ] Keep our code **inside `USER CODE` blocks** or in separate files (blocker 3).
- [ ] After any `.ioc` change: regenerate, then check `App/` still builds (label renames break `board.c`).
- [ ] The generated `vApplicationStackOverflowHook` and malloc-failed hook are empty. Put a breakpoint or `Error_Handler()` in them, otherwise an overflow is silent.
- [ ] `defaultTask` (CubeMX) runs `osDelay(1)` forever. Remove it or make it `osDelay(osWaitForever)`.
- [ ] Check every `osThreadNew` / `osMessageQueueNew` / `osMutexNew` result and call `Error_Handler()` on `NULL` (see how `Game_Init` does it).
- [ ] Stack sizes (bytes in CMSIS): tasks that call `snprintf`/`printf` need about 2 KB, simple tasks 512 B. Heap is 32 KB.

### Timing and fairness (the point of the project)
- [ ] **Timestamp in the interrupt, not in the task.** Reading `Board_NowUs()` in the task adds scheduling delay and can reverse the order of two near-simultaneous presses.
- [ ] Decide the winner by **timestamp comparison**, with a short wait for the second player ([game.c:236](../App/Src/game.c#L236)).
- [ ] **Early presses:** flush the player queues right before the cue, and ignore events older than the cue. Decide whether an early press should be penalised (the md says no).
- [ ] **Nobody reacts:** a round needs a timeout, otherwise the game waits forever (`ROUND_TIMEOUT_*` in `game_config.h`).
- [ ] **Exact tie:** nobody scores (decide if you agree).
- [ ] **Debounce** buttons and switches (30 ms in `board.c`). A held button or a switch already in the wrong position must not count.
- [ ] Interrupts that call RTOS functions need NVIC priority 5 or numerically higher (already set in CubeMX). Use timeout 0 inside ISRs.

### Inputs
- [ ] **Pot semantics are undefined in the md.** "Twist" = moved far enough from the starting position (threshold 1000 of 4095)? or reached an end stop? Needs a decision and a test with the real pot.
- [ ] Start `HAL_ADC_Start_DMA` and disable the DMA interrupt, otherwise it fires ~200 000 times per second ([board.c:84-90](../App/Src/board.c#L84)).
- [ ] A slide switch has no "pressed" state: any edge is a flip. A switch that rests in the middle or bounces needs the debounce.
- [ ] Wiring: buttons and switches go to **GND** with the internal pull-up (CubeMX already set it). Pots to 3V3, never 5 V.

### Outputs
- [ ] **7-segment:** common anode or common cathode (`SEG_COMMON_ANODE` in `board.c`), and segment order. Byte order of the two chained 595s is player 1 first, because the chip chained to the first one (player 1, left) is the last in the chain ([board.c:141-150](../App/Src/board.c#L141)).
- [ ] Score range is **0-9** per display, so `WIN_SCORE` must be <= 9.
- [ ] **Buzzer** is a passive piezo: it needs a frequency (PWM), a plain GPIO high makes no sound. The timer prescaler must be 83 (1 MHz) or the pitch is wrong.
- [ ] **One owner per output:** only the score task touches the displays, only the round manager the buzzer, only one task the UART. Otherwise two tasks write at once.
- [ ] LED resistors for the 3 cue LEDs (or 6 if the pairs are not wired together) and 14 display segments.

### Game rules to decide
- [ ] What counts as a **wrong input**: ignored (current) or penalty?
- [ ] Is the time limit shrinking per round? Minimum?
- [ ] Is the random wait before the cue wanted? (`CUE_DELAY_*`)
- [ ] Can the same input appear twice in a row?
- [ ] How does a game **start and restart**: blue button B1 (current) or one of the player inputs?
- [ ] Per-player **reaction-time statistics** (best / average over rounds): the assignment text asks for it and **nobody has it yet** (only the single-round reaction time is printed, [game.c:355-361](../App/Src/game.c#L355)). Needs a stats struct next to the scores (mutex again).
- [ ] Randomness: no hardware RNG on the F401. Seed from the timer at the start press; do not use `rand()` from several tasks.

### Testing
- [ ] Smoke test before the game: each input lights an LED and beeps, scores count on the displays ([docs/PINOUT.md](PINOUT.md) section 6 and the earlier smoke-test idea).
- [ ] UART debug output (115200, ST-LINK) shows every round: use it to check timing claims.
- [ ] Test the **simultaneous press** case (both players press within a few ms) and the **early press** case on purpose.
- [ ] Optional: a UART "fake input" layer to test the game logic before the hardware is assembled.

## 4. Who does what (suggestion)

| Area | Files |
|---|---|
| Hardware layer (pins, 595, buzzer, ADC, EXTI) | `App/Src/board.c` |
| Game flow (round, I/O manager, players) | `App/Src/game.c` or the `main.c` skeleton |
| Scores, statistics | `App/Src/score.c` |
| Rules and tunables | `App/Inc/game_config.h`, [GAME.md](GAME.md) |
