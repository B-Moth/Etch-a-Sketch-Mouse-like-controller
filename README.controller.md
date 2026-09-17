# flemme.ai — Etch A Sketch USB Controller

A physical Etch A Sketch-style controller that drives any mouse-based web drawing app over USB, with an animated OLED face as display.

## Content of the folder

- this **README.controller.md**
- **kicad_v2** folder containing the design for the PCB (Printed Circuit Board) used for this part of the project
- **Controller_box.3mf** 3D file containing the model printed for the case
- **etch_a_sketch_controller_v4** folder containing the code (C and C++ mainly) flashed on the MCU (MicroController Unit)

---

## Hardware

| Component | Role |
|-----------|------|
| ATmega32U4 (Pro Micro) | Microcontroller — native USB HID |
| 2× EC11 rotary encoder | Left knob = X axis, right knob = Y axis |
| Toggle switch | Pen down (drawing) / pen up (moving) |
| GY-521 MPU-6050 | Accelerometer — detects shake gesture |
| SSD1306 OLED 128×64 | Animated face display |

The OLED and the MPU-6050 share the I2C bus (SDA/SCL, pins 2/3) at different addresses: `0x3C` and `0x68`.

---

## How it works

### Drawing
The controller presents itself to the computer as a **standard USB HID mouse** — no driver, no companion app needed. Any web drawing tool that responds to mouse events works out of the box.

- Rotating encoder X sends `Mouse.move(dx, 0)` — moves the cursor horizontally
- Rotating encoder Y sends `Mouse.move(0, dy)` — moves the cursor vertically
- Flipping the toggle switch sends `Mouse.press(MOUSE_LEFT)` or `Mouse.release()`, toggling the drawing stroke on and off

### Shake to clear
The MPU-6050 is polled at 100 Hz. The firmware counts **direction reversals** in the acceleration signal above a threshold (≈ 0.5 g). A deliberate shake — at least 6 reversals sustained over 1.5 seconds — triggers a **Ctrl+A → Delete** keyboard sequence, clearing the canvas. A 3-second cooldown prevents accidental re-triggers. This requires the web app to support select-all + delete (Excalidraw and tldraw both do).

### OLED face
The display refreshes at ~30 fps and shows one of two states:

**Normal face** — a circular face with white eyes and filled pupils. The pupils track the direction of the last cursor movement using a smoothed velocity vector that decays back to center when idle (~300 ms). The face blinks at random intervals (every 2–6 seconds).

**Dizzy face** — triggered on shake, shown for 5 seconds. The eyes are replaced by rotating Archimedean spirals (animated), and the mouth becomes a crooked line.

On boot, a **startup screen** plays the `flemme.ai` logo.

---

## Wiring summary

```
Pro Micro          Component
─────────────────────────────
Pin 4 (CLK) ───── Encoder X
Pin 5 (DT)  ───── Encoder X
Pin 6 (CLK) ───── Encoder Y
Pin 7 (DT)  ───── Encoder Y
Pin 8       ───── Toggle switch (other leg → GND)
Pin 2 (SDA) ───── OLED SDA  +  MPU-6050 SDA
Pin 3 (SCL) ───── OLED SCL  +  MPU-6050 SCL
3.3V        ───── OLED VCC  +  MPU-6050 VCC
GND         ───── all grounds
```

Wiring was helped with the design and etching of a PCB, the case for the controller was designed and printed with Bambu suite.
All was done and manufactured at the LabElec of 42Paris

---

## Sources used

- First and main source of both knowledge and infrastructure was the **LabElec of 42Paris** and the support of the **42Chips** association's members.
- As we use a clone of the Arduino Leonardo MCU, https://docs.arduino.cc was our main source of official documentation.
- In the same way, https://forum.arduino.cc and similar forums helped a great deal at every stepts and layer of this part of the project.
- IA models such as Claude were used to debug both hardware and software, as well as to help write part of this document.
