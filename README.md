# RobotHead — ESP32-CAM Robot Head Firmware

Camera stream + 14-servo control + status LEDs for an animatronic robot head.

---

## 1. What this is

Firmware for an **AI-Thinker ESP32-CAM** that does three things at once:

1. **Video** — serves a web UI and MJPEG stream over WiFi.
2. **Servo control** — drives **14 servos** through a **PCA9685** PWM driver (I2C), accepting target angles over UDP.
3. **Status LEDs** — the red LED blinks in patterns that tell you what the board is doing.

The ESP32 does **not** decide the poses itself. It listens on UDP port 4210 and moves each servo toward the latest target angle it received.

---

## 2. STEP-BY-STEP — do these in order

### Step 1 — Safety check first (do this before anything else)

Plug **only** the bare ESP32-CAM into power (no PCA9685, no servos, no wires on the pin headers) and watch the temperature for 30 seconds.

- **Warm** (regulator chip around 60-70 °C — hot but you can keep a finger on it) = normal. The small 3-legged AMS1117 regulator always runs hot on these boards.
- **Too hot to touch within a few seconds, burning, smell, or discoloration** = STOP. Unplug. Go to **§8 Power & overheating** before continuing. Do not flash or wire anything until it cools and you've found the cause.

### Step 2 — Prepare the files

Get the files (GitHub **Code → Download ZIP**, then extract, or `git clone`). Keep **all files in one folder** next to `RobotHead.ino` — the sketch will not compile if `app_httpd.cpp`, `camera_index.h`, `board_config.h` or `camera_pins.h` are elsewhere.

### Step 3 — Install Arduino IDE + ESP32 board support

1. Install Arduino IDE (1.8.x or 2.x).
2. File → Preferences → Additional Boards URL:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Tools → Board → Boards Manager → install **esp32** by Espressif.

### Step 4 — Install the two libraries

Sketch → Include Library → Manage Libraries, install:

1. **Adafruit PWM Servo Driver Library**
2. **Adafruit BusIO** (installed automatically as a dependency, install manually if not)

### Step 5 — Set your WiFi

Open `RobotHead.ino`, lines **46-47**:

```c
const char *ssid = "YOUR_WIFI_NAME";
const char *password = "YOUR_WIFI_PASSWORD";
```

Must be a **2.4 GHz** network — ESP32 cannot see 5 GHz.

### Step 6 — Select board settings

Tools menu:

| Setting | Value |
|---|---|
| Board | **AI Thinker ESP32-CAM** |
| PSRAM | **Enabled** |
| Partition Scheme | **Huge APP (3MB No OTA)** |
| Upload Speed | 115200 (raise to 921600 if upload is slow) |

### Step 7 — (Optional) Apply the 2 LED edits

Makes the red LED fast-blink while video streams. Everything works without it — LED just stays solid. See **§5**.

### Step 8 — Wire for flashing

ESP32-CAM has **no USB port**. Use a USB-TTL adapter:

| USB-TTL | ESP32-CAM |
|---|---|
| 5V | 5V |
| GND | GND |
| TX | U0R (RX) |
| RX | U0T (TX) |

**Then: GPIO0 → GND** (this puts the board in flashing mode). Keep it connected during upload only.

### Step 9 — Upload

1. Select the correct **Port** (Tools → Port).
2. Press **Upload**.
3. If it fails: check TX/RX are not swapped, hold GPIO0→GND, press **Reset** on the board, try again.
4. When done: **remove the GPIO0→GND jumper** and press Reset.

### Step 10 — Verify it boots

1. Tools → Serial Monitor, set baud **115200**.
2. Press Reset. You should see:
   ```
   WiFi connected
   Camera Ready! Video: http://192.168.x.x
   ```
3. Note that IP address. If the red LED fast-blinks forever, camera init failed — recheck Step 6 (PSRAM must be Enabled).

### Step 11 — Open the video in a browser

On a device on the **same WiFi network**, open `http://<the IP>`. You get the control page + live stream. `/stream` is the raw MJPEG URL.

### Step 12 — Wire the servo driver

Power off first. Connect PCA9685 to the ESP32-CAM:

| ESP32-CAM pin | PCA9685 pin |
|---|---|
| **5V** | **V+** (servo power — use a separate strong 5V supply for the servos) |
| **3.3V** | **VCC** (logic) |
| **GPIO 13** | **SDA** (`SERVO_SDA`) |
| **GPIO 14** | **SCL** (`SERVO_SCL`) |
| GND | GND (common ground **required**) |

Then verify **`PCA_ADDR`** in `RobotHead.ino:57` matches your board's solder jumpers (`0x60` in code; chip default is `0x40`).

### Step 13 — Send angles (the missing piece)

Nothing in this project sends servo commands. Run a sender on your PC — this moves joint 0 to 45°:

```python
import socket
ESP_IP = "192.168.x.x"        # the IP from Step 10
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
pose = [90] * 14
pose[0] = 45
sock.sendto(bytes([1] + pose), (ESP_IP, 4210))
```

Full protocol in **§6**. Servos should ease to the target (default 150°/s max).

### Step 14 — Set limits and save a boot pose

1. Tighten per-joint limits in `servoBegin()` so joints can't over-rotate:
   `jointMin[3] = 20; jointMax[3] = 160;`
2. Send `[2]` to store the current pose as the power-on pose:
   `sock.sendto(bytes([2]), (ESP_IP, 4210))`
   (white LED pulses to acknowledge)

---

## 3. File list

| File | Purpose |
|---|---|
| `RobotHead.ino` | Main sketch: WiFi, camera init, 14-servo control, UDP listener, LED status |
| `app_httpd.cpp` | HTTP server: `/` control UI, `/stream` video, `/capture`, `/control` |
| `camera_index.h` | Gzipped web UI page (the camera control interface) |
| `board_config.h` | Board selection — currently **AI Thinker** |
| `camera_pins.h` | Camera pin maps for each supported board |
| `partitions.csv` | Custom partition table (3.75 MB app slot) |
| `ci.yml` | CI build matrix (arduino-cli board options) |
| `README.md` / `README.docx` | This documentation |

---

## 4. Wiring summary (PCA9685 servo driver)

| ESP32-CAM pin | PCA9685 pin | Note |
|---|---|---|
| **5V** | **V+** | Servo power rail. 14 servos can draw several amps — use a dedicated 5V supply, never the ESP32 board. |
| **3.3V** | **VCC** | Logic supply. ESP32 is 3.3V logic. |
| **GPIO 13** | **SDA** | I2C data — `SERVO_SDA` |
| **GPIO 14** | **SCL** | I2C clock — `SERVO_SCL` |
| GND | GND | Common ground required |

```c
#define SERVO_SDA   13
#define SERVO_SCL   14
#define PCA_ADDR    0x60   // chip default is 0x40 — match your jumpers
#define NUM_SERVOS  14
```

---

## 5. Optional LED edits

`RobotHead.ino` expects two edits in `app_httpd.cpp` so the red LED can show **"video streaming"** (fast blink). Skip freely — no other feature depends on this.

1. Near the top of `app_httpd.cpp` (after the includes, around line 25):
   ```c
   extern volatile uint32_t lastStreamFrameMs;
   ```
2. Inside the `while(true)` loop of `stream_handler()`, right after `int64_t fr_end = esp_timer_get_time();` (line ~281) add:
   ```c
   lastStreamFrameMs = millis();
   ```

---

## 6. UDP servo protocol — port 4210

Binary packets sent to the ESP32's IP, port **4210**:

| Packet | Meaning |
|---|---|
| `[1, a0, a1, ... a13]` | Set 14 target angles, each **0-180** (clamped to joint limits) |
| `[2]` | Save the current pose as the **startup pose** (stored in flash) |
| `[3, s]` | Set max speed, `s` = degrees per 20 ms tick (**1-20**) |

Behaviour:

- Newest packet wins; packets are queued and drained each cycle.
- Each servo moves toward its latest target at no more than `maxStep` degrees every 20 ms (default 3.0 = 150 deg/s).
- Movement **never stops halfway** if packets pause — it finishes to the last target.
- Joints default to full **0-180**; tighten them in `servoBegin()` to protect the mechanics.

---

## 7. LED status codes

### Red LED (GPIO 33, active LOW)

| Pattern | Meaning |
|---|---|
| Very fast blink | Camera init failed |
| Slow blink | Connecting / WiFi lost |
| Solid | Connected, waiting |
| Fast blink | Video streaming *(needs the §5 edits)* |
| Double pulse | Angle feed active |
| Triple pulse | Feed stopped within the last 10 s |

### White flash LED (GPIO 4)

- Short flash at boot (self-test)
- Low glow while the angle feed is active
- Brighter pulse when a "save pose" packet arrives

---

## 8. Power & overheating

**What's normal:** on an ESP32-CAM the small 3-legged **AMS1117 voltage regulator** (5V → 3.3V) runs genuinely hot — ~70 °C — in normal operation. So does the ESP32 module under load and the camera sensor. Warm = fine.

**What's NOT normal:** anything on the board reaching a burning/`too hot to touch` temperature within a few seconds of plugging in. That means excess current — a short or a failed part.

### Test sequence

1. **Unplug and let it cool** (2-3 minutes).
2. **Strip down:** remove the PCA9685, all servo wires, GPIO0 jumper, SD card and anything on the pin headers. Power the bare board only.
3. **Still overheats in seconds** → fault on the board itself:
   - Look for **solder bridges / bent pins** on the camera ribbon connector and header pins.
   - Look for debris, flux residue or moisture under the board and around the camera socket.
   - Common failure: the **AMS1117 fails shorted** and can push 5V onto the 3.3V rail — that cooks the ESP32 and camera. If you have a multimeter, check the 3.3V pin: should read **3.2-3.4 V**. Anything above ~4 V = dead regulator, board needs repair/replacement.
   - Also common: a **shorted ceramic capacitor** on the 3.3V rail.
4. **Bare board stays cool, heats only when the PCA9685/servos are connected** → wiring fault:
   - **V+ and VCC swapped** (5V into the 3.3V logic rail will kill the driver).
   - Servo supply **back-feeding** into the ESP32 through the 5V pin — power servos separately, common ground only.
   - Stalled/miswired servo drawing huge current.
5. **Only the USB-TTL adapter chip is warm** → normal for CH340/CP2102 adapters. Only worry if it too is too hot to touch.
6. **If the board already overheated badly**, don't flash it — expect possible camera/sensor damage even if it boots.

---

## 9. Troubleshooting

| Symptom | Likely cause |
|---|---|
| Red LED fast-blinks forever | Camera init failed — check board selection / PSRAM |
| Red LED slow-blinks | Wrong WiFi name/password, or 5 GHz network |
| No page opens | Note IP from Serial Monitor; PC must be on the same network |
| Upload fails | Hold **GPIO0 to GND** during flashing, then Reset; check TX/RX not swapped |
| Compile error about `psramFound` / large image | Partition Scheme not set to **Huge APP (3MB No OTA)** |
| `PCA9685 not found` in serial log | Wiring (SDA/SCL) or wrong `PCA_ADDR` |
| Camera fine but servos never move | No UDP sender, wrong `PCA_ADDR`, V+/GND not connected, or VCC/V+ swapped |
| Board too hot to touch | **§8 Power & overheating** |
| Image noisy | Drop `config.xclk_freq_hz` from 20 MHz to 10 MHz (`RobotHead.ino`) |

---

## 10. To-do checklist

- [ ] Step 1: heat check passed (bare board only warm)
- [ ] Step 5: WiFi credentials set
- [ ] Step 6: board + PSRAM + partition selected
- [ ] Step 8-9: flashed, GPIO0 jumper removed
- [ ] Step 10: serial log shows `Camera Ready!` + IP
- [ ] Step 11: video opens in browser
- [ ] Step 12: PCA9685 wired, `PCA_ADDR` verified
- [ ] Step 13: UDP sender moves a servo
- [ ] Step 14: joint limits tightened, boot pose saved
- [ ] §5: optional LED edits applied
