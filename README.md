# 🏎️ ESP32 Mobile Wi-Fi Remote Control Car (Pure Motor Drive)

Control your ESP32 robot car directly from your smartphone (**iPhone or Android**) with **zero app downloads**! The ESP32 hosts its own Wi-Fi Access Point and serves an ultra-fast web cockpit with touch controls, speed regulation, straight-line steering trim, and active dynamic electromagnetic braking.

---

## 📋 Wiring Reference

### L298N Motor Driver to ESP32
> ⚠️ **CRITICAL:** Remove the two black jumper caps on the **ENA** and **ENB** pins of the L298N so the ESP32 can send PWM speed signals!

| L298N Pin | ESP32 Pin | Function | Description |
| :--- | :--- | :--- | :--- |
| **ENA** | **GPIO 25 (D25)** | Left Motors PWM Speed | Removes jumper cap; controls left motor speed (0–255) |
| **IN1** | **GPIO 26 (D26)** | Left Direction 1 | Digital Output |
| **IN2** | **GPIO 27 (D27)** | Left Direction 2 | Digital Output |
| **IN3** | **GPIO 14 (D14)** | Right Direction 1 | Digital Output |
| **IN4** | **GPIO 18 (D18)** | Right Direction 2 | Digital Output |
| **ENB** | **GPIO 33 (D33)** | Right Motors PWM Speed | Removes jumper cap; controls right motor speed (0–255) |

---

### Motor Terminal Connections:
* **Left Motors:** Wire to L298N **OUT1** and **OUT2** screw terminals.
* **Right Motors:** Wire to L298N **OUT3** and **OUT4** screw terminals.

---

## ⚡ Power Connections

Motors pull high peak currents (up to 1.5A–2A). **Do not power motors directly from the ESP32 USB port or 3.3V pin.**

### Recommended Power Setup:
Use **two 18650 Li-ion rechargeable batteries in series (7.4V – 8.4V)** or a 2S/3S LiPo battery.

```
       [ 7.4V - 8.4V Battery Pack ]
         (+)                   (-)
          |                     |
          |       +-------------+
          |       |             |
          v       v             |
      +---------------+         |
      | 12V      GND  |         |
      |               |         |
      |     L298N     |         |
      |   5V Out      |         |
      +-------+-------+         |
              |                 |
              v                 v
          [ VIN ]            [ GND ]
          +-------------------------+
          |         ESP32           |
          +-------------------------+
```

1. **Common Ground:** Connect Battery Negative (`-`), L298N `GND`, and ESP32 `GND` together.
2. **L298N 5V Jumper:** Keep the onboard 5V regulator jumper on the L298N **plugged in**.
3. **ESP32 Power:** Connect the L298N's **5V Out terminal** to the ESP32's **VIN (or 5V)** pin.

---

## 📲 How to Connect & Drive from Your Mobile

1. **Upload Sketch:** Open [`esp32_rc_car/esp32_rc_car.ino`](file:///Users/apple/remot%20control%20car/esp32_rc_car/esp32_rc_car.ino) in Arduino IDE and click **Upload (➡️)**.
2. **Turn on the Car:** Switch on your battery pack.
3. **Turn OFF Mobile Data** on your phone *(Crucial so Chrome doesn't route traffic over 4G/5G)*.
4. **Connect Phone to Wi-Fi:**
   - Network Name (SSID): `ESP32-RC-CAR`
   - Password: `password123`
5. **Open the Controller Web App:**
   - Open Safari or Chrome and navigate to:
     ```
     http://192.168.4.1
     ```
6. **(Optional) Add to Home Screen:** Tap Share / Options $\rightarrow$ **Add to Home Screen** to run it in full screen without the browser address bar!

---

## 🕹️ Dashboard Layout & Features

```
+------------------------------------------+
| 🏎️ ESP32 ROVER            🟢 CONNECTED   |
+------------------------------------------+
| [Motor Speed (Power)]               75%  |
| 🐢 [-----------●---------] ⚡  (190 PWM)  |
| [ PRECISE ]     [ CRUISE ]     [ BOOST ] |
+------------------------------------------+
| [Straight-Line Steering Trim]   0 Center |
| ◀ L [----------●---------] R ▶          |
| [ ◀ Trim L ]    [ RESET ]    [ Trim R ▶ ]|
+------------------------------------------+
| [Direction Pad]                          |
|         [ ↖ ]     [ ▲ FWD ]     [ ↗ ]    |
|       [ ⟲ L ]     [ ■ STOP ]   [ ⟳ R ]   |
|         [ ↙ ]     [ ▼ REV ]     [ ↘ ]    |
+------------------------------------------+
```

- **Touch & Hold to Drive:** Touch and hold any arrow button to drive. Release to stop immediately.
- **Active Electromagnetic Dynamic Braking:** Halts wheel momentum on a dime with zero rolling or coasting overshoot.
- **Straight-Line Steering Trim:** Live slider to balance Left/Right motor power so the car drives in a laser-straight line.
- **Calibrated Turning:** Controlled pivot turning (⟲ ⟳) and smooth arc steering (↖ ↗).
- **Watchdog Failsafe:** Stops motors automatically if Wi-Fi signal drops for > 2.5 seconds.
