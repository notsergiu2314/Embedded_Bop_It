6 inputs (3 per player) 4 digital 2 analog
2 seg displays 2 digital 1 clock
1 buzzer 1 digital 
6 LED outputs (3 per player) 6 digital

13 digital
2 analog




# Software layout

Inputs: Player 1 pot but swi, Player 2 pot but swi
Outputs: Player 1 score, Player 2 score, Buzzer, Player 1 lights, Player 2 lights

1. Round manager: sets up the basic gameplay, inits values, starts up the first round, keeps doing rounds until winning score reached
2. I/O manager: Determines a random input, waits for it from both players
3. Player 1 task: keeps track of P1 input
4. Player 2 task: keeps track of P2 input
5. Score manager: keeps track of score mutex, updates 7 seg displays




## For each round:

Buzzer beeps once indicating a start

a single input LED turns on for a random  input
(exactly the same for both players)

waits for the player to input smth

then compares results

gives a point to winner
update displays

maybe buzz?

next round


## Inidivdiaul tasks

### Round manager

1. keep track of whcih round it is
2. Keep track of whether either player has a score above threshold
3. Buzzer control
4. Notify/start I/O manager once per round, wait
5. Notify score manager for every score update/ update score (not sure)
6. Break if game won

### I/O manager

0. Determine random input
1. Light up corresponding LEDs
2. Starts P1 and P2 tasks, send desired input to both, waits for both
3. Once the first one or both return,
4. Make sure both player tasks are idle
5. Calculate winner
6. Winner indication to HW (clear LEDs, maybe flash something, maybe buzz)
7. Notify round manager with winner

### Player tasks

1. Prepare I/O pins for correct input. Incorrect inputs ignored
2. Wait until correct input
3. notify I/O manager that correct input was placed (either bool or timetsamp)

### Score manager

1. wait for notif with new scores
2. read scores, send them to 7 segs


# input tasks

There should be 6 input tasks, 3 for each player

each task corresponds toT

# PINOUTS
## Inputs
 - P1 Button - D2
 - P1 Switch - D5
 - P1 Pot - A0
   
 - P2 Button - D7
 - P2 Switch - D8
 - P2 Pot - A1
## Outputs
### Player LEDs
 - Button - D9
 - Switch - D15
 - Pot - D14
### Shift registers

SER (Data) - D4  
SRCLK (Clock) - D3  
RCLK (Latch) - D6  

### Buzzer
(VCC) - D10  
<!-- 
Two 74HC595 chained (DIP-16 pin numbers):

| 595 pin | Chip 1 (player 1) | Chip 2 (player 2) |
|---|---|---|
| 16 VCC | 3V3 | 3V3 |
| 8 GND | GND | GND |
| 13 OE | GND | GND |
| 10 SRCLR | 3V3 | 3V3 |
| 14 SER (data in) | PB5 (D4) | pin 9 of chip 1 |
| 11 SRCLK (clock) | PB3 (D3) | PB3 (D3) |
| 12 RCLK (latch) | PB10 (D6) | PB10 (D6) |
| 9 QH' (data out) | -> chip 2 pin 14 | unused |
| 15, 1, 2, 3, 4, 5, 6 = QA..QG | segments a, b, c, d, e, f, g | segments a, b, c, d, e, f, g |
| 7 QH | decimal point (optional) | decimal point (optional) | 
-->
