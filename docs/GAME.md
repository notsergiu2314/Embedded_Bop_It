# Game design and software structure

Source: `App/Src/game.c` (Round manager, I/O manager, player tasks), `App/Src/score.c`
(Score manager) and `App/Src/board.c` (hardware). Every tunable number is in
`App/Inc/game_config.h`.

## How a game runs

1. Press the blue button **B1** to start. Scores go to 0-0.
2. **Each round:**
   1. The buzzer beeps once (round starts).
   2. After a random wait (0.5-2 s) one **cue LED** turns on, the **same input for both players**
      (button, switch or potentiometer).
   3. Each player must use that input: press the button, flip the switch, or turn the pot.
      Any other input is ignored.
   4. The first valid reaction wins the point. The winner's LEDs flash, the buzzer gives feedback
      and the displays update.
   5. If nobody reacts within the time limit, nobody gets the point (low beep).
3. The first player to reach `WIN_SCORE` (5) wins: fanfare and blinking LEDs. Press B1 to play again.

The time limit starts at 3000 ms and shrinks by 100 ms every round, down to 1000 ms.

## Tasks

| Task | Priority | What it does |
|---|---|---|
| **Round manager** | Normal | Counts rounds, start beep, hands each round to the I/O manager, adds the point, checks `WIN_SCORE`, buzzer, prints the round summary on the serial port. |
| **I/O manager** | Above normal | Picks the random input, lights the LEDs, starts both player tasks, waits for the first result, judges, shows the result, tells the Round manager. |
| **Player 1 / Player 2** | Above normal | Idle until commanded. Waits for the requested input of *its* player, ignores the others, reports a timestamp. One function, two instances. |
| **Score manager** | Below normal | Owns the scores (behind a mutex). When notified, reads them and updates both 7-segment displays. |

```
 Round manager --RoundRequest--> I/O manager --PlayerCmd--> Player 1 task <-- input queue 1 <-- EXTI
       ^                              ^   |                 Player 2 task <-- input queue 2 <-- EXTI
       |                              |   +-- abort flag ---->      |
       +----- RoundResult ------------+<--- PlayerResult + DONE flag
 Round manager --Score_Add--> Score manager --> 7-segment displays
```

| Link | Mechanism |
|---|---|
| Round manager -> I/O manager | queue `roundReqQueue` (round number, time limit) |
| I/O manager -> Round manager | queue `roundResultQueue` (winner, reaction times) |
| I/O manager -> player task | queue `playerCmdQueue[p]` (target input, cue time) |
| player task -> I/O manager | queue `playerResultQueue` (player, timestamp) + thread flag "done" |
| I/O manager -> player task | thread flag `PLAYER_FLAG_ABORT` (stop waiting) |
| interrupt -> player task | queue `playerInputQueue[p]` (button / switch edge with timestamp) |
| anyone -> Score manager | thread flag (scores changed); data behind `scoreMutex` |
| B1 interrupt -> Round manager | binary semaphore |

## Who is first

* Button and switch times are captured **in the interrupt** (1 us timer). The pot is polled every
  ~1 ms, so its time is accurate to about 1 ms.
* After the first player reacts, the I/O manager waits `NEAR_TIE_WINDOW_MS` (10 ms) for the other,
  and then compares the **timestamps**, not the order in which the tasks happened to run.
* Exactly equal timestamps = nobody scores.
* Before every cue the player queues are emptied, and events older than the cue are discarded,
  so a button pressed *before* the cue never counts as a fast reaction.

## Choices not in the original design (change them in `game_config.h` or `game.c`)

* Random 0.5-2 s wait between the start beep and the cue, so nobody can anticipate it.
* A time limit per round that shrinks, and a round without any reaction is a draw. Without it the
  game would wait forever.
* The blue button B1 starts and restarts the game.
* **Wrong inputs are ignored** (no penalty), as in the design. Pressing early is also ignored.
  A penalty (e.g. a point for the other player) could be added in the player task.
* **Pot input** = the pot moved at least `POT_MOVE_THRESHOLD` (1000 of 4095 counts, about a quarter
  of the range) from where it was when the cue came on, in either direction.
* `WIN_SCORE` is 5. The maximum is 9, because each player has a single digit.

## Serial output (115200 baud)

```
=== Bop-It === press the blue button to start (first to 5 wins)
Round 1 (3000 ms): SWITCH -> P2 | P1 412.8 ms, P2 305.1 ms | score 0-1
Round 2 (2900 ms): POT -> nobody | P1 --, P2 -- | score 0-1
```
