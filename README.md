# Flicker-Free LCD Display Engine (ESP32 + Wokwi)

An LCD rendering engine for a 16x2 I2C display. It never clears the screen. It keeps a
copy of what is on the display and writes only the characters that changed. Four pages
rotate on a non-blocking timer, and any critical fault immediately takes over the screen
until the operator clears it.

▶ **Run it live:**<https://wokwi.com/projects/476952611904086017>
video <https://lnkd.in/p/gxW7NK9P>


## Features
- **No flicker:** `lcd.clear()` is never called after start-up; unchanged characters are never rewritten
- **Shadow buffer:** compares the wanted frame with the on-screen copy and writes only the differences
- **Non-blocking:** `millis()` timers only, no `delay()`; characters written per loop pass are capped
- **4 auto-rotating pages** every 3 s: Battery, System state, Telemetry, LCD statistics
- **Critical fault override:** the fault screen appears within one 50 ms sample, bypassing the timers, and stays until an explicit ACK
- **Display dead-band:** tiny sensor wobble does not make digits flutter
- Live counters show how many characters were saved compared with a clear-and-redraw

## Files
| File | Purpose |
|---|---|
| `sketch.ino` | Firmware |
| `diagram.json` | Wokwi circuit |
| `libraries.txt` | Wokwi library list (`LiquidCrystal I2C`) |
| `REPORT.md` | Design report, including the refresh-interval justification |

## Hardware (Wokwi)
| Part | Pin | Role |
|---|---|---|
| Pot 1 | GPIO 34 | Battery voltage, 9.0 to 12.6 V |
| Pot 2 | GPIO 35 | Load current, 0 to 10 A |
| Pot 3 | GPIO 32 | Temperature, 20 to 80 °C |
| I2C LCD 16x2 | SDA 21, SCL 22 | Display |
| Green / yellow / red LED | GPIO 4 / 5 / 2 | Normal / warning / fault |
| Buzzer | GPIO 18 | Pulses during a fault |
| Relay (active LOW) | GPIO 19 | Opens during a fault |
| Button NEXT | GPIO 25 | Jump to the next page |
| Button ACK | GPIO 26 | Clear a fault |
| Button TEST | GPIO 27 | Raise a manual test fault |

## How to run
1. Open the Wokwi link, or create an ESP32 project and paste in `sketch.ino`,
   `diagram.json` and `libraries.txt`.
2. Press Play. Turn the pots and watch the LCD, with the Serial Monitor open.

## The pages
| Page | Line 1 | Line 2 |
|---|---|---|
| 1 Battery | `Batt 11.0V  56%` | Charge bar `[#######------]` |
| 2 System | `State:NORMAL` (or WARNING) | `Relay:ON  Flt:0` |
| 3 Telemetry | `I: 3.0A T:30°C` | `Up 00:01:25` (uptime) |
| 4 LCD stats | `Wr:3742 Mv:1330` | `Saved 90%` |

The last character of line 2 shows the page number.

## The fault screen
| Cause | Trips when | Clears when |
|---|---|---|
| OVCURR | Current above 8.0 A | Below 7.0 A |
| OVTEMP | Temperature above 60 °C | Below 55 °C |
| UNDERV | Voltage below 9.3 V | Above 9.6 V |
| TEST | TEST button | ACK |

```
!!FAULT!! OVTEMP
Fix cause, ACK
```
- It replaces the normal pages immediately and page rotation stops.
- The fault is **latched**: it stays even after the cause goes away.
- It clears only when the cause is gone **and** you press ACK. If you press ACK while the
  cause is still present, the screen says `Cause STILL ON!`.
- If several faults are active, the highest priority is shown: OVCURR, OVTEMP, UNDERV, TEST.
- The relay opens and the buzzer pulses until the fault is cleared.

## How the engine works
1. A **frame buffer** holds the text we want. Each page writes into it, never straight to the LCD.
2. A **shadow buffer** holds what the LCD currently shows.
3. `flush()` scans the two buffers, moves the cursor to the first difference, and writes
   only the characters that changed. If two changes are one character apart it writes
   through the gap, because a cursor move costs as much as a character.
4. It writes at most 6 characters per pass and remembers where it stopped, so the loop never blocks.
5. A fault bypasses the cap and writes all 32 characters in the same pass, so it appears at once.

## Test results
Tested on a PC against a simulated LCD that records every write (not on Wokwi or real hardware):
- A 0.3 V change rewrote 8 characters instead of 32
- Sensor wobble of ±0.02 V wrote nothing
- Pages rotated every 3000 ms
- Fault screen appeared 45 ms after the fault (simulated time; I2C transfer time not included)
- Fault stayed after the cause went away, ACK refused while the cause was present, and ACK returned to the normal pages
- 10 minutes of random pot and button input: the real screen always matched the shadow buffer
- `lcd.clear()` was called 0 times
- In a 5-minute steady run it wrote 12.5 characters/s, compared with 128 characters/s for clear-and-redraw at the same refresh rate (about 90% fewer)

## Limitations
- Timing figures for the real I2C bus are estimates, not measurements
- The fault threshold uses raw readings with hysteresis but no debounce; real sensors would need one
- The library's own short waits (tens of microseconds per character) still happen inside each write

## License
MIT
