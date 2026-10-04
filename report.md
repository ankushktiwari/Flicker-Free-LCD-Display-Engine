# Flicker-Free LCD Display Engine: Project Report

## 1. Objective
Build an LCD rendering engine for an ESP32 and a 16x2 HD44780 I2C display that:
- updates only the parts of the screen whose values changed, with no clearing or visible flicker;
- rotates automatically through at least three information pages on a non-blocking timer;
- overrides the display immediately on a critical fault and shows a dedicated fault screen until the fault is explicitly cleared;
- uses a refresh interval that is justified by the LCD's characteristics and the importance of the data.

## 2. Hardware (Wokwi)
| Part | Pin | Purpose |
|---|---|---|
| ESP32 DevKit | n/a | Controller |
| 3 potentiometers | GPIO 34, 35, 32 | Battery voltage, load current, temperature (stand-ins for real sensors) |
| I2C LCD 16x2 | SDA 21, SCL 22 | Display |
| Green / yellow / red LED | GPIO 4 / 5 / 2 | Normal / warning / fault |
| Buzzer | GPIO 18 | Fault alarm |
| Relay (active LOW) | GPIO 19 | Opens on fault |
| 3 pushbuttons | GPIO 25 / 26 / 27 | Next page / acknowledge / test fault |

## 3. Why LCDs flicker
The usual approach is `lcd.clear()` followed by printing everything again. This causes problems:
- **Blanking:** the clear command erases every character and takes about 1.5 ms inside the controller, so the whole screen goes empty before it is redrawn.
- **Slow redraw:** the display is written one character at a time over I2C, so a full 32-character redraw takes tens of milliseconds, during which the screen is partly old and partly new.
- **Wasted work:** most characters (labels, units, unchanged digits) are rewritten with the same value.
- **Visible effect:** the text shimmers or blinks, especially on a camera, and the I2C bus is busy far more than necessary.

The fix is to treat the LCD as a slow output device and send it only what has changed.

## 4. Rendering engine design

### 4.1 Two buffers
| Buffer | Holds |
|---|---|
| `frame[2][16]` | What the screen should show. Each page composes into this off-screen buffer, so building a page never touches the LCD. |
| `shadow[2][16]` | What the screen currently shows, updated only when a character is actually written. |

### 4.2 Differential flush
`flush(budget)` walks the 32 character cells:
1. Skip cells where `frame` equals `shadow`.
2. At the first difference, move the cursor once and write the changed characters in a row.
3. If two changes are only one character apart, write through the gap. A cursor move costs about as much as one character, so merging is cheaper than moving twice.
4. Update `shadow` for every character written.
5. Stop after `budget` characters and remember the scan position.

The sketch calls `lcd.clear()` zero times after start-up. The LCD's own initialisation clears it once.

### 4.3 Non-blocking write budget
Each loop pass writes at most 6 characters (about 6 to 8 ms of bus time, estimated), then returns to the main loop. A page change that alters 30 characters is spread over about 5 passes. Because the loop runs far faster than that, the change still appears in a few tens of milliseconds. Nothing waits and no `delay()` is used.

### 4.4 Preventing digit flutter
A sensor reading that jitters across a rounding boundary (for example 11.04 to 11.05 V) would change a digit every refresh. Two measures prevent this:
- Values are smoothed with a first-order filter before display.
- A display dead-band keeps the shown value fixed until the real value moves by at least 0.07 V, 0.1 A or 0.5 °C.

The dead-band applies to the display only. Fault decisions use the raw readings.

## 5. Page rotation (non-blocking)
A `millis()` timestamp switches the page every 3000 ms. The NEXT button also advances the page and restarts the timer.

| Page | Content | Information type |
|---|---|---|
| 1 Battery | Voltage, charge %, bar graph | Battery status |
| 2 System | NORMAL or WARNING, relay state, fault count | System state |
| 3 Telemetry | Current, temperature, uptime | Telemetry |
| 4 LCD stats | Characters written, cursor moves, % saved | Engine diagnostics |

Page changes are rendered immediately on the timer tick rather than waiting for the next refresh, so a page always appears on time.

## 6. Critical fault override
### 6.1 Behaviour
- Every 50 ms the sketch checks the raw readings against fault limits with hysteresis (OVCURR above 8.0 A, OVTEMP above 60 °C, UNDERV below 9.3 V). A TEST button raises a manual fault.
- On a new fault the sketch composes the fault screen and writes all 32 characters in the same loop pass. It does not wait for the page timer, the refresh timer or the 6-character budget.
- While a fault is active the page timer and NEXT button are ignored.
- The relay opens and the buzzer pulses.

### 6.2 Latched until explicitly cleared
The fault stays latched after the cause disappears. It clears only when:
1. the cause is gone, and
2. the operator presses ACK.

Pressing ACK while the cause is still present shows `Cause STILL ON!` for two seconds and the fault stays. The second line of the fault screen alternates every 2 s between the live value (for example `T=72°C >60°C`) and the instruction (`Fix cause, ACK` or `Cause gone: ACK`).

### 6.3 Fault screen layout
```
!!FAULT!! OVTEMP
T=72°C >60°C
```
Only the two `!!` marks blink (every 500 ms), so the blink changes 4 characters at most and the rest of the screen stays still. Multiple faults are shown by priority: OVCURR, OVTEMP, UNDERV, TEST.

### 6.4 Latency
Worst-case time from fault to fault screen is the 50 ms sample period plus the time to write 32 characters (estimated 35 to 45 ms). That is roughly 100 ms at most, which is well inside a typical human reaction time of about 250 ms. In the PC simulation the screen appeared 45 ms after the fault; that run does not include I2C transfer time.

## 7. Choice of refresh interval: 250 ms

The sketch separates four rates, each chosen for a different reason:

| Rate | Value | Reason |
|---|---|---|
| Sensor and fault check | 50 ms | Fault detection should be fast, and sampling costs the display nothing |
| **Content refresh** | **250 ms (4 Hz)** | Described below |
| Page dwell | 3000 ms | Time to read two lines of text with margin |
| Fault screen | Event-driven, not timed | Safety information must not wait for a timer |

### 7.1 Why 250 ms for the routine refresh
1. **LCD response time.** Character LCDs of this type use twisted-nematic or STN liquid crystal. Their pixel response is typically on the order of 100 to 300 ms at room temperature, and slower when cold (check the datasheet for your module). Changes faster than this smear instead of showing a clean new value, so updating faster than about 4 Hz brings no visible benefit.
2. **Human reading speed.** A number that changes more than about four times a second cannot be read. A 4 Hz update still feels live but lets the eye settle on each value.
3. **Rate of change of the data.** Battery voltage and temperature move over seconds or minutes. Load current can change faster, but a number on a small display is a status indication, and 250 ms is quick enough for an operator to follow it. Precise fast signals belong in logging, not on a 16x2 screen.
4. **Bus and CPU cost.** Because only changed characters are written, the typical refresh touches 0 to 6 characters. In the 5-minute steady PC test the engine wrote about 12.5 characters per second, including page changes. At an estimated 1 ms per I2C character (about 100 kHz bus), that is a bus load of a few percent. A clear-and-redraw at the same rate would write 128 characters per second plus four clear commands, roughly 18% bus load and a blank screen four times a second.
5. **Consistency with the sample rate.** 250 ms is a multiple of the 50 ms sample period (every 5th sample), so the screen always shows fresh data without aliasing.

### 7.2 Why not another value
| Interval | Problem |
|---|---|
| 50 to 100 ms | Faster than the LCD can respond and faster than a person can read. Digits blur and the bus works harder for no benefit. |
| 250 ms | Matches LCD response, readable, low bus load. **Chosen.** |
| 500 ms | Acceptable for voltage and temperature, but current changes feel sluggish and the operator may see a value up to half a second old. |
| 1000 ms or more | Visibly stale for load changes. Acceptable only for non-critical telemetry such as uptime. |

### 7.3 Importance of the information
Information is ranked by consequence:
- **Critical (fault):** overrides every timer and appears within about 100 ms. Its latency is set by the 50 ms sample period, not by the refresh interval.
- **Important (battery, current, temperature, state):** refreshed at 4 Hz, which is as fast as is useful.
- **Low (uptime, engine statistics):** shown on pages that rotate every 3 s. Their digits change once a second or less, and a slower rate would be enough.

This separation is the main point of the design: the routine interval is tuned for readability and LCD response, while safety information is event-driven so a slow refresh cannot delay it.

### 7.4 How to retune
`REFRESH_MS`, `PAGE_MS`, `SAMPLE_MS`, `CHAR_BUDGET` and `MERGE_GAP` are constants at the top of the sketch. With a colder environment or a slower LCD, raise `REFRESH_MS` to 300 to 400 ms.

## 8. Test results
Tested on a PC with stubbed Arduino functions and a simulated LCD that records every cursor move and character write. This was not run on Wokwi or real hardware.

| Test | Result |
|---|---|
| Battery page shows the right value; real screen equals shadow buffer | Pass |
| Pages rotate | Intervals of 2995, 3000, 3003 ms |
| 0.3 V change | 8 characters written (not 32) |
| Sensor wobble of ±0.02 V for 2 s | 0 characters written |
| Over-temperature fault | Fault screen 45 ms after the fault; relay opened |
| Fault screen stays 10 s with no page rotation | Pass |
| Fault stays after the cause is gone | Pass (latched) |
| ACK while the cause is present | Refused, shows `Cause STILL ON!` |
| ACK after the cause is gone | Returns to the normal pages, relay closed |
| Priority (OVCURR above OVTEMP), manual TEST fault | Pass |
| 10 minutes of random pot and button input | Real screen always equal to the shadow buffer; 0 loop stalls |
| `lcd.clear()` calls | 0 |
| Passes over the 6-character budget | Only fault entry and exit |
| Steady run (5 min, fixed values) | 12.5 chars/s written vs 128 chars/s for clear-and-redraw (about 90% saved) |

The saving is lower during heavy changes (about 82% in the random test) because more characters genuinely change.

## 9. Limitations and future work
- I2C timing figures (about 1 ms per character at 100 kHz) are estimates from how the library drives the PCF8574, not measurements. Measure on real hardware.
- The LiquidCrystal I2C library still waits tens of microseconds inside each character write. This is short, and the character budget keeps the total per pass small.
- The fault thresholds use hysteresis but no time debounce. Real sensors should add one.
- Custom characters (a proper block bar graph and a degree symbol) could replace the `#` bar.
- A larger display (20x4) would need only the buffer size constants changed, since the flush works on any grid.

## 10. Conclusion
The engine meets the brief. The screen is never cleared; only changed characters are sent; four pages rotate on a `millis()` timer; and a critical fault takes over the display immediately and stays until the operator acknowledges it after the cause is gone. The 250 ms routine refresh matches the LCD's response and human reading speed, and safety information bypasses it entirely.
