// ================================================================
// ETCH A SKETCH USB CONTROLLER + OLED FACE
//
// Hardware:
//   - Pro Micro / Leonardo (ATmega32U4)
//   - 2x EC11 rotary encoders
//   - Toggle switch
//   - MPU6050 (GY-521)
//   - SSD1306 OLED 128x64 I2C
//
// Features:
//   - Encoders -> mouse movement
//   - Switch -> mouse button
//   - Shake -> clear canvas
//   - OLED eyes follow movement direction
//   - OLED dizzy face after shake
// ================================================================

#include <Mouse.h>
#include <Keyboard.h>
#include <Wire.h>
#include <math.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define DEBUG_SERIAL 0

#if DEBUG_SERIAL
#define DBG_BEGIN(baud) Serial.begin(baud)
#define DBG_PRINT(...) Serial.print(__VA_ARGS__)
#define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
#define DBG_BEGIN(baud)
#define DBG_PRINT(...)
#define DBG_PRINTLN(...)
#endif

// ================================================================
// PINS
// ================================================================

#define ENC_X_CLK 4
#define ENC_X_DT  5

#define ENC_Y_CLK 7
#define ENC_Y_DT  6

#define SWITCH_PIN 8

#define MPU_I2C_ADDR 0x68

// ================================================================
// OLED
// ================================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  -1
);

// ================================================================
// SETTINGS
// ================================================================

#define MOUSE_SPEED        10

#define ENCODER_DETENT_THRESHOLD 0
// number of quadrature transitions per mechanical detent (usually 4)
#define ENCODER_STEPS_PER_DETENT 4

// If an encoder is wired reversed, set invert to 1 to flip direction
#define ENC_X_INVERT 0
#define ENC_Y_INVERT 0

// debounce threshold for quick opposite transitions (microseconds)
#define ENCODER_BOUNCE_US 800

#define SHAKE_THRESHOLD    0.5f
#define SHAKE_MIN_PEAKS    6
#define SHAKE_MIN_DURATION 1500
#define SHAKE_WINDOW       2500
#define SHAKE_COOLDOWN     3000

#define IMU_POLL_MS        10

// ================================================================
// STATE
// ================================================================

int8_t lastXA = 0;
int8_t lastYA = 0;
bool penDown = false;

// Shake detector

int8_t shakeLastSign = 0;
int shakePeakCount = 0;

uint32_t shakeStart = 0;
uint32_t lastShakeTrig = 0;

// Face

int eyeOffsetX = 0;
int eyeOffsetY = 0;

int lastDX = 0;
int lastDY = 0;

bool dizzyFace = false;
uint32_t dizzyUntil = 0;

// ------------------------------------------------
// Blink animation
// ------------------------------------------------

bool blinking = false;
uint32_t nextBlink = 0;
uint32_t blinkEnd = 0;

// ------------------------------------------------
// Dizzy animation
// ------------------------------------------------

uint8_t dizzyFrame = 0;

// (encoder used to use simple edge-based handling)

// Quadrature decoder state (2-bit: CLK<<1 | DT)
uint8_t lastXState = 0;
uint8_t lastYState = 0;

int8_t accX = 0;
int8_t accY = 0;

// debounce trackers
uint32_t lastXDeltaMicros = 0;
int8_t lastXDeltaSign = 0;

uint32_t lastYDeltaMicros = 0;
int8_t lastYDeltaSign = 0;

// quadrature transition table: index = (prev<<2)|curr
// values: -1 = CCW, +1 = CW, 0 = no/invalid
const int8_t QUAD_TABLE[16] = {
  0, -1,  1,  0,
  1,  0,  0, -1,
 -1,  0,  0,  1,
  0,  1, -1,  0
};

// ================================================================
// FACE DRAWING
// ================================================================

void drawNormalFace() {

  display.clearDisplay();

  // head

  display.drawCircle(64, 39, 23, WHITE);

  // smile

  display.drawCircle(64, 40, 10, WHITE);

  display.fillRect(
    52,
    26,
    24,
    16,
    BLACK
  );

  // eyes

  if (blinking) {

    display.drawLine(46, 24, 62, 24, WHITE);
    display.drawLine(66, 24, 82, 24, WHITE);

  } else {

    display.fillCircle(54, 24, 8, WHITE);
    display.fillCircle(74, 24, 8, WHITE);

    display.fillCircle(
      54 + eyeOffsetX,
      24 + eyeOffsetY,
      3,
      BLACK
    );

    display.fillCircle(
      74 + eyeOffsetX,
      24 + eyeOffsetY,
      3,
      BLACK
    );
  }

  display.display();
}

void drawDizzyFace() {

  display.clearDisplay();

  display.drawCircle(64, 39, 23, WHITE);

  // left spiral

  for (int i = 0; i < 10; i++) {

    float a =
      (i + dizzyFrame * 0.25) * 0.8;

    int x =
      54 + cos(a) * i;

    int y =
      32 + sin(a) * i;

    display.drawPixel(x, y, WHITE);
  }

  // right spiral

  for (int i = 0; i < 10; i++) {

    float a =
      (i + dizzyFrame * 0.25) * 0.8;

    int x =
      74 + cos(a) * i;

    int y =
      32 + sin(a) * i;

    display.drawPixel(x, y, WHITE);
  }

  // crooked mouth

  display.drawLine(
    54,
    48,
    74,
    44,
    WHITE
  );

  display.display();
}

void updateEyes() {

  eyeOffsetX = 0;
  eyeOffsetY = 0;

  if (lastDX > 0)
    eyeOffsetX = 3;

  if (lastDX < 0)
    eyeOffsetX = -3;

  if (lastDY > 0)
    eyeOffsetY = 3;

  if (lastDY < 0)
    eyeOffsetY = -3;
}

// ================================================================
// MPU6050
// ================================================================

void mpuInit() {

  Wire.beginTransmission(MPU_I2C_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission(true);
}

float readAccelDelta() {

  Wire.beginTransmission(MPU_I2C_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);

  Wire.requestFrom(MPU_I2C_ADDR, 6, true);

  int16_t rawX =
    ((int16_t)Wire.read() << 8) | Wire.read();

  int16_t rawY =
    ((int16_t)Wire.read() << 8) | Wire.read();

  int16_t rawZ =
    ((int16_t)Wire.read() << 8) | Wire.read();

  float gx = rawX / 16384.0f;
  float gy = rawY / 16384.0f;
  float gz = rawZ / 16384.0f;

  float magnitude =
    sqrtf(gx * gx + gy * gy + gz * gz);

  return magnitude - 1.0f;
}

// readEncoderDelta removed — using simple edge detection instead

// ================================================================
// SHAKE DETECTOR
// ================================================================

bool updateShake(float delta) {

  uint32_t now = millis();

  if ((now - lastShakeTrig) < SHAKE_COOLDOWN)
    return false;

  int8_t sign = 0;

  if (delta > SHAKE_THRESHOLD)
    sign = 1;

  if (delta < -SHAKE_THRESHOLD)
    sign = -1;

  if (sign != 0 && sign != shakeLastSign) {

    if (shakeLastSign != 0) {

      if (shakePeakCount == 0)
        shakeStart = now;

      shakePeakCount++;
    }

    shakeLastSign = sign;
  }

  if (shakePeakCount > 0 &&
      (now - shakeStart) > SHAKE_WINDOW) {

    shakePeakCount = 0;
    shakeLastSign = 0;

    return false;
  }

  if (shakePeakCount >= SHAKE_MIN_PEAKS &&
      (now - shakeStart) >= SHAKE_MIN_DURATION) {

    shakePeakCount = 0;
    shakeLastSign = 0;

    lastShakeTrig = now;

    return true;
  }

  return false;
}

// ================================================================
// CLEAR SHORTCUT
// ================================================================

void sendClearShortcut() {

  Keyboard.press(KEY_LEFT_CTRL);
  Keyboard.press('a');

  delay(60);

  Keyboard.releaseAll();

  delay(60);

  Keyboard.press(KEY_DELETE);

  delay(60);

  Keyboard.releaseAll();
}

// ================================================================
// SETUP
// ================================================================

void setup() {

  DBG_BEGIN(115200);

  #if DEBUG_SERIAL
  while (!Serial && millis() < 2500) {
    ;
  }
  DBG_PRINTLN(F("Booting Etch-A-Sketch controller"));
  DBG_PRINTLN(F("Ready!"));
  #endif

  pinMode(ENC_X_CLK, INPUT_PULLUP);
  pinMode(ENC_X_DT, INPUT_PULLUP);

  pinMode(ENC_Y_CLK, INPUT_PULLUP);
  pinMode(ENC_Y_DT, INPUT_PULLUP);

  pinMode(SWITCH_PIN, INPUT_PULLUP);

  lastXA = digitalRead(ENC_X_CLK);
  lastYA = digitalRead(ENC_Y_CLK);

  // keep lastXA/lastYA as initial CLK readings
  lastXA = digitalRead(ENC_X_CLK);
  lastYA = digitalRead(ENC_Y_CLK);

  // initialize quadrature states
  lastXState = (digitalRead(ENC_X_CLK) << 1) | digitalRead(ENC_X_DT);
  lastYState = (digitalRead(ENC_Y_CLK) << 1) | digitalRead(ENC_Y_DT);

  Wire.begin();

  mpuInit();

  #if DEBUG_SERIAL
  DBG_PRINTLN(F("MPU6050 wake command sent"));
  #endif

  if (!display.begin(
        SSD1306_SWITCHCAPVCC,
        0x3C)) {

    #if DEBUG_SERIAL
    DBG_PRINTLN(F("ERROR: OLED init failed at 0x3C"));
    #endif

    while (1);
  }

  #if DEBUG_SERIAL
  DBG_PRINTLN(F("OLED init OK"));
  #endif

  display.clearDisplay();
  display.display();

  Mouse.begin();
  Keyboard.begin();

  randomSeed(micros());

  nextBlink = millis() + random(2000, 6000);
}

// ================================================================
// LOOP
// ================================================================

void loop() {

  // ------------------------------------------------
  // X ENCODER
  // ------------------------------------------------

  // --- X encoder (quadrature) ---
  {
    uint8_t DX = digitalRead(ENC_X_CLK);
    uint8_t DY = digitalRead(ENC_X_DT);

    // uint8_t state = (a << 1) | b; // 0..3
    // uint8_t prev = lastXState;
    // uint8_t idx = (prev << 2) | state; // 0..15

    // int8_t delta = QUAD_TABLE[idx];
    // lastXState = state;

    #if DEBUG_SERIAL
    if (state != prev) {
      DBG_PRINT(F("X enc prev=")); DBG_PRINT(prev);
      DBG_PRINT(F(" curr=")); DBG_PRINT(state);
      DBG_PRINT(F(" idx=")); DBG_PRINT(idx);
      DBG_PRINT(F(" delta=")); DBG_PRINTLN(delta);
      delay(5);
    }
    #endif

    if (DX != lastDX)
    {
      Mouse.move(DX, 0, 0);
    }
    lastDX = DX;
    lastDY = 0;


    // if (ENC_X_INVERT) delta = -delta;

    // if (delta != 0) {
    //   uint32_t now = micros();
    //   int8_t sign = (delta > 0) ? 1 : -1;

      // ignore quick opposite-sign transitions (bounce)
      // if (lastXDeltaSign != 0 && sign != lastXDeltaSign &&
      //     (now - lastXDeltaMicros) < ENCODER_BOUNCE_US) 
      // {
      //   #if DEBUG_SERIAL
      //   DBG_PRINT(F("X bounce ignored (delta=")); DBG_PRINT(delta);
      //   DBG_PRINT(F(" elapsed=")); DBG_PRINT(now - lastXDeltaMicros);
      //   DBG_PRINTLN(F("us)"));
      //   #endif
      // } 
      // else 
      // {
    //     accX += delta;
    //     lastXDeltaMicros = now;
    //     lastXDeltaSign = sign;

    //     // when we accumulate full detent steps, emit movement
    //     if (abs(accX) >= ENCODER_STEPS_PER_DETENT) {
    //       int8_t clicks = accX / ENCODER_STEPS_PER_DETENT;
    //       int8_t dx = clicks * MOUSE_SPEED;

    //       Mouse.move(dx, 0, 0);

    //       accX -= clicks * ENCODER_STEPS_PER_DETENT;

    //       #if DEBUG_SERIAL
    //       DBG_PRINT(F("Encoder X: delta=")); DBG_PRINT(delta);
    //       DBG_PRINT(F(" acc=")); DBG_PRINT(accX);
    //       DBG_PRINT(F(" dx=")); DBG_PRINTLN(dx);
    //       #endif

    //       lastDX = dx;
    //       lastDY = 0;
    //     }
    //   //}
    // }
  }

  // ------------------------------------------------
  // Y ENCODER
  // ------------------------------------------------

  // --- Y encoder (quadrature) ---
  {
    uint8_t a = digitalRead(ENC_Y_CLK);
    uint8_t b = digitalRead(ENC_Y_DT);

    uint8_t state = (a << 1) | b; // 0..3
    uint8_t prev = lastYState;
    uint8_t idx = (prev << 2) | state; // 0..15

    int8_t delta = QUAD_TABLE[idx];
    lastYState = state;

    #if DEBUG_SERIAL
    if (state != prev) {
      DBG_PRINT(F("Y enc prev=")); DBG_PRINT(prev);
      DBG_PRINT(F(" curr=")); DBG_PRINT(state);
      DBG_PRINT(F(" idx=")); DBG_PRINT(idx);
      DBG_PRINT(F(" delta=")); DBG_PRINTLN(delta);
      delay(5);
    }
    #endif

    if (ENC_Y_INVERT) delta = -delta;

    if (delta != 0) {
      uint32_t now = micros();
      int8_t sign = (delta > 0) ? 1 : -1;

      // if (lastYDeltaSign != 0 && sign != lastYDeltaSign &&
      //     (now - lastYDeltaMicros) < ENCODER_BOUNCE_US) {
      //   #if DEBUG_SERIAL
      //   DBG_PRINT(F("Y bounce ignored (delta=")); DBG_PRINT(delta);
      //   DBG_PRINT(F(" elapsed=")); DBG_PRINT(now - lastYDeltaMicros);
      //   DBG_PRINTLN(F("us)"));
      //   #endif
      // } else {
        accY += delta;
        lastYDeltaMicros = now;
        lastYDeltaSign = sign;

        if (abs(accY) >= ENCODER_STEPS_PER_DETENT) {
          int8_t clicks = accY / ENCODER_STEPS_PER_DETENT;
          int8_t dy = clicks * MOUSE_SPEED;

          Mouse.move(0, dy, 0);

          accY -= clicks * ENCODER_STEPS_PER_DETENT;

          #if DEBUG_SERIAL
          DBG_PRINT(F("Encoder Y: delta=")); DBG_PRINT(delta);
          DBG_PRINT(F(" acc=")); DBG_PRINT(accY);
          DBG_PRINT(F(" dy=")); DBG_PRINTLN(dy);
          #endif

          lastDX = 0;
          lastDY = dy;
        }
      //}
    }
  }

  // ------------------------------------------------
  // PEN SWITCH
  // ------------------------------------------------

  bool switchClosed =
    (digitalRead(SWITCH_PIN) == LOW);

  if (switchClosed && !penDown) {

    Mouse.press(MOUSE_LEFT);
    penDown = true;

  } else if (!switchClosed && penDown) {

    Mouse.release(MOUSE_LEFT);
    penDown = false;
  }

  // ------------------------------------------------
  // IMU
  // ------------------------------------------------

  static uint32_t lastIMURead = 0;

  uint32_t now = millis();

  if (now - lastIMURead >= IMU_POLL_MS) {

    float delta = readAccelDelta();

    if (updateShake(delta)) {

      sendClearShortcut();

      dizzyFace = true;
      dizzyUntil = millis() + 5000;
    }

    lastIMURead = now;
  }

  // ------------------------------------------------
  // FACE
  // ------------------------------------------------
  if (!blinking &&
    millis() > nextBlink) {

  blinking = true;

  blinkEnd =
    millis() + 150;
  }

  if (blinking &&
      millis() > blinkEnd) {

    blinking = false;

    nextBlink =
      millis() + random(2000, 6000);
  }
  updateEyes();
  dizzyFrame++;
  if (dizzyFace) {

    drawDizzyFace();

    if (millis() > dizzyUntil)
      dizzyFace = false;

  } else {

    drawNormalFace();
  }
}