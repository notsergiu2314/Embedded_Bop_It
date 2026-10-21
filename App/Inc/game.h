/* game.h - Round manager, I/O manager and player tasks. */

#ifndef GAME_H
#define GAME_H

/* Creates all queues and tasks of the game (also calls Board_Init and
 * Score_Init). Call once after osKernelInitialize(), before osKernelStart(). */
void Game_Init(void);

#endif /* GAME_H */
