/*
  ESP32-CAM robot head: camera stream + 14-servo control (PCA9685) + status LEDs

  Board:      AI Thinker ESP32-CAM, PSRAM enabled, Partition Scheme "Huge APP (3MB No OTA)"
  Libraries:  Adafruit PWM Servo Driver Library (+ Adafruit BusIO)
  Keep app_httpd.cpp, camera_index.h, board_config.h and camera_pins.h in this folder.
  board_config.h must have CAMERA_MODEL_AI_THINKER enabled.

  ONE EDIT IS NEEDED in app_httpd.cpp so the red LED can show "video streaming":
    1) at the top of the file:
         extern volatile uint32_t lastStreamFrameMs;
    2) inside the while(true) loop of stream_handler(), right after a frame is sent
       (next to the line "int64_t fr_end = esp_timer_get_time();"):
         lastStreamFrameMs = millis();

  UDP packets to port 4210 (binary):
    [1, a0 .. a13]  set target angles, 0-180 each (clamped to the joint limits below)
    [2]             save the current pose as the startup pose
    [3, s]          set max speed, s = degrees per 20 ms tick (1-20)

  The ESP does not care where the angles come from (live mirroring or scripted steps).
  It moves each servo toward the latest target at no more than maxStep degrees per tick.

  Red LED (GPIO33, active LOW):
    very fast blink  camera init failed        slow blink    connecting / WiFi lost
    solid            connected, waiting        fast blink    video streaming
    double pulse     angle feed active         triple pulse  feed stopped in the last 10 s
  White flash LED (GPIO4): low glow while the angle feed is active, short flash at boot,
  brighter pulse when a "save pose" packet arrives.
*/

#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_PWMServoDriver.h>

// Select camera model in board_config.h
#include "board_config.h"

// ===========================
// WiFi credentials (2.4 GHz)
// ===========================
const char *ssid = "FD-15";
const char *password = "1234567890";

void startCameraServer();
void setupLedFlash();

// ===========================
// Servo control
// ===========================
#define SERVO_SDA   13
#define SERVO_SCL   14
#define PCA_ADDR    0x60      // set to match the board's address jumpers (default for the chip is 0x40)
#define NUM_SERVOS  14
#define UDP_PORT    4210

Adafruit_PWMServoDriver pca(PCA_ADDR, Wire1);
Preferences prefs;
WiFiUDP udp;

int servoMinUs = 500;
int servoMaxUs = 2500;

uint8_t jointMin[NUM_SERVOS];
uint8_t jointMax[NUM_SERVOS];
float cur[NUM_SERVOS];
int lastUs[NUM_SERVOS];
volatile uint8_t tgt[NUM_SERVOS];
volatile float maxStep = 3.0f;          // degrees per 20 ms tick (150 deg/s)
volatile uint32_t lastPacket = 0;       // millis() of the last angle packet
volatile uint32_t ackUntil = 0;         // white-LED acknowledgement window

void saveCalibration(const int *angles) {
  prefs.begin("robot_cal", false);
  for (int i = 0; i < NUM_SERVOS; i++) {
    prefs.putInt(("ch" + String(i)).c_str(), angles[i]);
  }
  prefs.end();
}

void writeServo(int i) {
  int us = servoMinUs + (int)((cur[i] / 180.0f) * (servoMaxUs - servoMinUs));
  if (us != lastUs[i]) {                // only touch the bus when the value changes
    pca.writeMicroseconds(i, us);
    lastUs[i] = us;
  }
}

void servoTask(void *) {
  uint8_t buf[1 + NUM_SERVOS];
  for (;;) {
    // Read every queued packet; the newest angles win
    while (udp.parsePacket() > 0) {
      int len = udp.read(buf, sizeof(buf));
      if (len < 1) continue;

      if (buf[0] == 1 && len >= 1 + NUM_SERVOS) {            // angles
        for (int i = 0; i < NUM_SERVOS; i++) {
          tgt[i] = constrain(buf[1 + i], jointMin[i], jointMax[i]);
        }
        lastPacket = millis();
      } else if (buf[0] == 2) {                              // save startup pose
        int a[NUM_SERVOS];
        for (int i = 0; i < NUM_SERVOS; i++) a[i] = (int)cur[i];
        saveCalibration(a);
        ackUntil = millis() + 300;
      } else if (buf[0] == 3 && len >= 2) {                  // set max speed
        maxStep = constrain((int)buf[1], 1, 20);
      }
    }

    // Always move toward the latest target (never stops halfway if packets pause)
    float ms = maxStep;
    for (int i = 0; i < NUM_SERVOS; i++) {
      float d = (float)tgt[i] - cur[i];
      cur[i] += constrain(d, -ms, ms);
      writeServo(i);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void servoBegin() {
  Wire1.begin(SERVO_SDA, SERVO_SCL, 400000);
  if (!pca.begin()) {                   // may log "Bus already started" - harmless
    Serial.println("PCA9685 not found (check wiring and PCA_ADDR)");
  }
  pca.setOscillatorFrequency(27000000);
  pca.setPWMFreq(50);

  // Joint limits: tighten per joint to protect the mechanics, e.g. jointMin[3] = 20; jointMax[3] = 160;
  for (int i = 0; i < NUM_SERVOS; i++) { jointMin[i] = 0; jointMax[i] = 180; }

  prefs.begin("robot_cal", true);
  for (int i = 0; i < NUM_SERVOS; i++) {
    int a = prefs.getInt(("ch" + String(i)).c_str(), 90);
    a = constrain(a, jointMin[i], jointMax[i]);
    cur[i] = a;
    tgt[i] = a;
    lastUs[i] = -1;
  }
  prefs.end();

  // Staggered soft start so all servos don't draw their surge at once
  for (int i = 0; i < NUM_SERVOS; i++) { writeServo(i); delay(60); }

  udp.begin(UDP_PORT);
  xTaskCreatePinnedToCore(servoTask, "servo", 6144, NULL, 2, NULL, 1);
}

// ===========================
// Status LEDs
// ===========================
#define RED_LED            33     // active LOW
#define WHITE_LED           4     // flash LED, PWM set up by setupLedFlash()
#define WHITE_FEED_DUTY    10     // 0-255, about 4%. Raise slowly: the LED is very bright and hot
#define WHITE_ACK_DUTY     40

volatile uint32_t lastStreamFrameMs = 0;   // updated by stream_handler() in app_httpd.cpp

void setRed(bool on) { digitalWrite(RED_LED, on ? LOW : HIGH); }

// n short pulses at the start of every 1200 ms cycle
bool pulsePattern(uint32_t now, int n) {
  uint32_t p = now % 1200;
  return p < (uint32_t)(n * 250) && (p % 250) < 100;
}

void updateLeds() {
  uint32_t now = millis();
  uint32_t lp = lastPacket;
  uint32_t ls = lastStreamFrameMs;
  bool wifiOk     = WiFi.status() == WL_CONNECTED;
  bool feedActive = lp != 0 && (now - lp) < 500;
  bool holding    = lp != 0 && !feedActive && (now - lp) < 10000;
  bool streaming  = ls != 0 && (now - ls) < 1000;

  if (!wifiOk)          setRed((now / 500) % 2 == 0);   // slow blink
  else if (feedActive)  setRed(pulsePattern(now, 2));   // double pulse
  else if (holding)     setRed(pulsePattern(now, 3));   // triple pulse
  else if (streaming)   setRed((now / 100) % 2 == 0);   // fast blink
  else                  setRed(true);                   // solid

  static bool whiteOwned = false;
  if (now < ackUntil) {
    ledcWrite(WHITE_LED, WHITE_ACK_DUTY);
    whiteOwned = true;
  } else if (feedActive) {
    ledcWrite(WHITE_LED, WHITE_FEED_DUTY);
    whiteOwned = true;
  } else if (whiteOwned) {
    ledcWrite(WHITE_LED, 0);            // hand the LED back to the web UI slider
    whiteOwned = false;
  }
}

// ===========================
// Setup / loop
// ===========================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  pinMode(RED_LED, OUTPUT);
  setRed(false);

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;           // 10 MHz also works if the image is noisy
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;     // keep JPEG: raw frames are far too big for WiFi
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;  // always the newest frame: lowest latency
  } else {
    config.frame_size = FRAMESIZE_QVGA;     // no PSRAM: keep frames small enough for DRAM
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.jpeg_quality = 15;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    while (true) { setRed(true); delay(80); setRed(false); delay(80); }   // very fast blink
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // sensor image is upside down by default
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  s->set_framesize(s, FRAMESIZE_VGA);       // good balance for processing; change in the web UI

#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif
  ledcWrite(WHITE_LED, 20); delay(150); ledcWrite(WHITE_LED, 0);   // white self-test flash

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    setRed((millis() / 500) % 2 == 0);      // slow blink
    delay(50);
  }
  Serial.println();
  Serial.println("WiFi connected");

  startCameraServer();
  servoBegin();

  Serial.print("Camera Ready! Video: http://");
  Serial.print(WiFi.localIP());
  Serial.print("  Stream: :81/stream  Servo UDP port: ");
  Serial.println(UDP_PORT);
}

void loop() {
  updateLeds();
  delay(50);
}
