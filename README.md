# Signal Roulette v1.3

We built Signal Roulette as our Communications 2 final project. It's a handheld ESP32 game that uses Clean, Noise, and Null signals as game mechanics. The console uses wired serial communication to drive its LCD and LED indicators.

## How to play

- Play solo against a Normal or Hard bot, or share the handheld with 2–4 local players.
- On your turn, send the next hidden signal from a shuffled six-signal batch to yourself or an opponent. The last player with HP wins the round. The batch reloads when it runs out or when no Noise signals remain.
- Set 1–5 rounds, 1–5 starting HP, an optional 5–30 second move timer, and whether to show the remaining signal counts.

| Signal | Effect |
| --- | --- |
| Noise | Removes 1 HP from the target unless a Firewall absorbs it. |
| Clean | Gives you another turn if you send it to yourself. |
| Null | Has no effect. |

**Items** include Scanner (reveal the next signal), Firewall (absorb a Noise hit), Recovery (restore 1 HP), and Block (skip the next player).

## Requirements

| Qty. | Component |
| --- | --- |
| 1 | ESP32 development board |
| 1 | 16×2 I²C LCD at address `0x27` |
| 2 | Daisy-chained 74HC595 shift registers |
| 12 | LEDs with current-limiting resistors (5 HP, 3 signal, and 4 item indicators) |
| 5 | Momentary push buttons |
| 1 | Passive buzzer for tone effects |
| As needed | Power supply and hookup wire |

Install the ESP32 board package in Arduino IDE and a `LiquidCrystal_I2C` library that supports `init()` and `backlight()`. The board package supplies `Wire`. Open `signal_roulette_v1.3.ino` and select your ESP32 board. The firmware uses ESP32 pin assignments, so an Arduino Uno would require remapping.

## ESP32 wiring and controls

| Connection | GPIO |
| --- | --- |
| LCD SDA / SCL (Generic ESP32 defaults) | 21 / 22 |
| Shift register data / latch / clock | 13 / 12 / 14 |
| Forward / Self | 4 |
| Backward / Opponent | 5 |
| Confirm / Select | 15 |
| Items | 18 |
| Pause / Back | 19 |
| Buzzer | 23 |
| Analog random seed (leave unconnected) | 34 |

Wire each button between its GPIO and ground. Use **Start Game** to configure a match, or open **Guide** for the rules. [Espressif I²C pin reference](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/i2c.html)

**Build note** — The supplied sketch uses a temporary array in the unused `tname` declaration inside `applySignal()`. If your compiler rejects that line, remove the declaration in a build copy. The function already uses `tnam` to format the player's name. [GCC compound literal documentation](https://gcc.gnu.org/onlinedocs/gcc/Compound-Literals.html)
