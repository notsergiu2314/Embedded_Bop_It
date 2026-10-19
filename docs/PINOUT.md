# Pinout - 2-player reaction game (NUCLEO-F401RE)

Each player has: 1 push button, 1 slide switch, 1 potentiometer, and one cue LED per input,
plus a 7-segment score display (0-9). Shared: 1 passive buzzer.
All 6 inputs go straight to the MCU (no input shift register), so the interrupt can timestamp
a press with 1 microsecond accuracy.

## 1. Board view (Arduino headers)

Pins are written `Arduino name  MCU pin`. `->` = MCU output, `<-` = MCU input, `~` = analog in.

```
 LEFT HEADERS                                      RIGHT HEADERS
 +----------------------------------+              +-----------------------------------+
 | CN6  power                       |              | CN5                               |
 |   3V3        -> 3V3 rail         |              |   D15  PB8   -> LED  P1 switch    |
 |   5V            (not used)       |              |   D14  PB9   -> LED  P1 pot       |
 |   GND        -> GND rail (x2)    |              |   AVDD                            |
 |   VIN, NRST, IOREF, NC (unused)  |              |   GND        -> GND rail          |
 +----------------------------------+              |   D13  PA5   LD2 on-board LED     |
 | CN8  analog                      |              |   D12  PA6   (free)               |
 |   A0   PA0   ~ pot  P1           |              |   D11  PA7   (free)               |
 |   A1   PA1   ~ pot  P2           |              |   D10  PB6   -> buzzer (TIM4 PWM) |
 |   A2   PA4   -> LED  P2 button   |              |   D9   PC7   -> LED  P1 button    |
 |   A3   PB0   -> LED  P2 switch   |              |   D8   PA9   <- switch P2         |
 |   A4   PC1   -> LED  P2 pot      |              +-----------------------------------+
 |   A5   PC0   (free)              |              | CN9                               |
 +----------------------------------+              |   D7   PA8   <- button P2         |
                                                   |   D6   PB10  -> 595 latch         |
   Not on the headers (ST-LINK side):              |   D5   PB4   <- switch P1         |
     PA2/PA3  USART2 TX/RX  (serial over USB)      |   D4   PB5   -> 595 data (MOSI)   |
     PC13     B1 blue button (free)                |   D3   PB3   -> 595 clock (SCK)   |
                                                   |   D2   PA10  <- button P1         |
                                                   |   D1   PA2   USART2 TX (ST-LINK)  |
                                                   |   D0   PA3   USART2 RX (ST-LINK)  |
                                                   +-----------------------------------+
```

## 2. Pin by pin

### Inputs

| MCU pin | Header | CubeMX mode / label | What it does |
|---|---|---|---|
| PA10 | D2 | GPIO_EXTI10, falling, pull-up, `BTN_P1` | Player 1 push button; the interrupt timestamps the press. |
| PB4 | D5 | GPIO_EXTI4, rising+falling, pull-up, `SW_P1` | Player 1 slide switch; any flip is a reaction. |
| PA0 | A0 | ADC1_IN0 | Player 1 potentiometer wiper (0-3.3 V). |
| PA8 | D7 | GPIO_EXTI8, falling, pull-up, `BTN_P2` | Player 2 push button; the interrupt timestamps the press. |
| PA9 | D8 | GPIO_EXTI9, rising+falling, pull-up, `SW_P2` | Player 2 slide switch; any flip is a reaction. |
| PA1 | A1 | ADC1_IN1 | Player 2 potentiometer wiper (0-3.3 V). |

### Cue LEDs (GPIO output, push-pull, start low, active high)

| MCU pin | Header | Label | What it does |
|---|---|---|---|
| PC7 | D9 | `LED_BTN_P1` | Lights when player 1 must press the button. |
| PB8 | D15 | `LED_SW_P1` | Lights when player 1 must flip the switch. |
| PB9 | D14 | `LED_POT_P1` | Lights when player 1 must turn the potentiometer. |
| PA4 | A2 | `LED_BTN_P2` | Lights when player 2 must press the button. |
| PB0 | A3 | `LED_SW_P2` | Lights when player 2 must flip the switch. |
| PC1 | A4 | `LED_POT_P2` | Lights when player 2 must turn the potentiometer. |

### Displays and buzzer

| MCU pin | Header | CubeMX mode / label | What it does |
|---|---|---|---|
| PB3 | D3 | SPI1_SCK | Shift clock to both 74HC595 chips. |
| PB5 | D4 | SPI1_MOSI | Serial data to the first 74HC595 (SPI mode: Transmit Only Master). |
| PB10 | D6 | GPIO output, `LATCH_595` | Rising edge copies the shifted bits to the display outputs. |
| PB6 | D10 | TIM4_CH1 (PWM) | Square wave for the passive buzzer; the frequency is the pitch. |

### Already used by the board

| MCU pin | CubeMX mode | What it does |
|---|---|---|
| PA2 / PA3 | USART2, 115200 | Debug printouts to the PC over the ST-LINK USB cable. |
| PA5 | GPIO output, `LD2` | On-board green LED (status, e.g. "game running"). |
| PC13 | GPIO_EXTI13, `B1` | On-board blue button, free for a "start / reset game" function. |
| PA13 / PA14 | SYS (SWD) | Programming and debugging, never change these. |

### Internal resources (no pin)

| Resource | Setting | What it does |
|---|---|---|
| TIM2 | prescaler 83, period 4294967295 | Free-running 1 MHz counter used for every timestamp. |
| TIM11 | SYS timebase | HAL tick (FreeRTOS owns SysTick). |
| TIM4 | prescaler 83, PWM CH1 | Buzzer pitch generator. |
| DMA2 Stream 0 | ADC1, circular | Keeps the two pot values up to date without CPU work. |

## 3. Wiring

* **Button:** one leg to the MCU pin, the other to GND (the internal pull-up does the rest).
* **Slide switch:** middle pin (common) to the MCU pin, one outer pin to GND.
* **Potentiometer:** outer pins to 3V3 and GND, middle pin (wiper) to the ADC pin. **Never 5 V.**
* **Cue LED:** MCU pin -> resistor -> LED anode, LED cathode -> GND.
* **7-segment display (assumed common cathode):** segments a-g through a resistor each to the 595 outputs, common pin to GND. For common anode, wire the common to 3V3 and set `SEG_COMMON_ANODE 1` in `board.c`.
* **Buzzer:** `+` to PB6, `-` to GND.
* **Resistors:** 14 for the segments (7 per display) + 6 for the LEDs = 20 in total.

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

Power the 595s from **3V3**, not 5 V, so their logic levels match the Nucleo.

## 4. CubeMX settings

1. **Board:** NUCLEO-F401RE, default peripherals. **SYS -> Timebase Source = TIM11.**
2. **FreeRTOS:** CMSIS_V2, tick 1000 Hz, heap 32768, enable `USE_TIMERS`, `USE_MUTEXES`, `USE_NEWLIB_REENTRANT`, `CHECK_FOR_STACK_OVERFLOW = 2`, `USE_MALLOC_FAILED_HOOK`.
3. **SPI1: change the pins before anything else!** CubeMX proposes PA5/PA6/PA7, which collides with LD2 and the free pins. Click the signals in the pin view and move **SCK to PB3 and MOSI to PB5**. Mode: *Transmit Only Master*, 8 bit, MSB first, prescaler 16, CPOL low, CPHA 1 Edge.
4. **TIM2:** clock source Internal Clock, prescaler **83**, counter period **4294967295**.
5. **TIM4:** Channel 1 = PWM Generation CH1 (pin PB6), prescaler **83**, counter period 999, pulse 0.
6. **ADC1:** IN0 and IN1, 12 bit, Scan Conversion **enabled**, Continuous Conversion **enabled**, DMA Continuous Requests **enabled**, 2 conversions (rank 1 = IN0, rank 2 = IN1), sampling time 84 cycles. Add DMA: **ADC1 -> DMA2 Stream 0, circular, Half Word / Half Word, memory increment on.**
7. **GPIO:** set the mode, pull and **User Label** of every pin as in the tables above. The labels create the `BTN_P1_Pin` / `BTN_P1_GPIO_Port` names in `main.h`.
8. **NVIC:** enable `EXTI line4`, `EXTI line[9:5]` and `EXTI line[15:10]` interrupts and give each a **preemption priority of 5 or higher (numerically)**. The default is 0, and with priority 0 FreeRTOS asserts the first time the interrupt calls `osMessageQueuePut`.
9. **Project Manager:** STM32CubeIDE, and tick *Generate peripheral initialization as a pair of .c/.h files* if you want smaller `main.c`.

## 5. How it looks in code

CubeMX turns the labels into this in `Core/Inc/main.h`:

```c
#define BTN_P1_Pin       GPIO_PIN_10
#define BTN_P1_GPIO_Port GPIOA
#define SW_P1_Pin        GPIO_PIN_4
#define SW_P1_GPIO_Port  GPIOB
#define LED_BTN_P1_Pin       GPIO_PIN_7
#define LED_BTN_P1_GPIO_Port GPIOC
#define LATCH_595_Pin       GPIO_PIN_10
#define LATCH_595_GPIO_Port GPIOB
/* ... one pair for every labelled GPIO pin ... */
```

`App/Src/board.c` is the only file that uses these names. It maps them to (player, input type) tables:

```c
static const Pin_t led_pin[PLAYER_COUNT][INPUT_COUNT] = {
    [PLAYER_1] = { [INPUT_BUTTON] = { LED_BTN_P1_GPIO_Port, LED_BTN_P1_Pin }, ... },
    [PLAYER_2] = { ... },
};
```

The rest of the program never mentions a pin. This is how the game code uses it
(see `docs/GAME.md` for the whole game):

```c
Board_Init(playerInputQueue);                   /* once; one queue per player */

Board_LedSet(PLAYER_1, INPUT_BUTTON, true);     /* cue: player 1 must press */
uint32_t cue_us = Board_NowUs();

InputEvent_t e;
if (osMessageQueueGet(playerInputQueue[PLAYER_1], &e, NULL, ticks) == osOK) {
    uint32_t reaction_us = e.timestamp_us - cue_us;   /* 1 us resolution */
    /* e.input tells which control the player used */
}

Board_DisplayScores(score1, score2);            /* 0..9 on the two displays */
Board_BuzzerTone(880);                          /* beep; Board_BuzzerTone(0) = silent */
Board_WaitStartButton(osWaitForever);           /* blue button B1 */
```

* Button and switch edges arrive as `InputEvent_t` in the queue **of the player they belong to**
  (timestamped in the interrupt, 30 ms debounce).
* The pot is **not** an event: the player task reads `Board_PotRead(player)` about every 1 ms.
* `Board_DisplayScores()` and the 595 latch are protected by a mutex, so any task may call them.
* The blue button B1 (PC13) releases a semaphore: `Board_WaitStartButton()` returns when it is pressed.

## 6. Things to check on the real board

* The pin map was chosen to avoid conflicts; CubeMX's pin view flags any clash, so check it once.
* PB3 is also the SWO trace pin on the Nucleo. It is fine for SPI as long as you don't use SWV trace output.
* `SEG_COMMON_ANODE` and the segment order (QA = a ... QG = g) depend on how you wire the display; adjust `seg_digit[]` if a digit looks wrong.
