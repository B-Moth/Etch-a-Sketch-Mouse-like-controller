// ================================================================
// ETCH A SKETCH USB CONTROLLER + OLED FACE (V3 CLEAN)
// ================================================================
// Refactor highlights:
// - constexpr configuration values
// - 30 FPS OLED refresh cap
// - Startup splash screen ("flemme.ai")
// - Safer MPU6050 reads
// - Time-based dizzy animation
// - Improved spiral rendering
// ================================================================

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

#define DEBUG_SERIAL 1

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

#define ENC_Y_CLK 6
#define ENC_Y_DT  7

#define SWITCH_PIN 8

#define MPU_I2C_ADDR 0x68

// ================================================================
// OLED
// ================================================================

constexpr uint8_t SCREEN_WIDTH = 128;
constexpr uint8_t SCREEN_HEIGHT = 64;

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  -1
);

// ================================================================
// SETTINGS
// ================================================================

constexpr int8_t MOUSE_SPEED = 4;

// If an encoder is wired reversed, set invert to 1 to flip direction
constexpr bool ENC_X_INVERT = false;
constexpr bool ENC_Y_INVERT = false;

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
int8_t prevXState = 0;
int8_t prevYState = 0;
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

// Display refresh
uint32_t lastDisplayRefresh = 0;
const uint16_t DISPLAY_FPS_MS = 33;

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

  if (blinking) 
  {

    display.drawLine(46, 24, 62, 24, WHITE);
    display.drawLine(66, 24, 82, 24, WHITE);

  } 
  else 
  {

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

  int px = 54;
  int py = 32;

  for (int i = 1; i < 40; i++) {

    float a = (i + dizzyFrame * 0.15f) * 0.55f;

    int x = 54 + (int)(cos(a) * i * 0.35f);
    int y = 32 + (int)(sin(a) * i * 0.35f);

    display.drawLine(px, py, x, y, WHITE);

    px = x;
    py = y;
  }

  // right spiral

  px = 74;
  py = 32;

  for (int i = 1; i < 40; i++) {

    float a = (i + dizzyFrame * 0.15f) * 0.55f;

    int x = 74 + (int)(cos(a) * i * 0.35f);
    int y = 32 + (int)(sin(a) * i * 0.35f);

    display.drawLine(px, py, x, y, WHITE);

    px = x;
    py = y;
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

void showStartupScreen() {

  display.clearDisplay();

  display.setTextColor(WHITE);

  for (int x = -90; x < 20; x += 3) {

    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(x, 24);
    display.print(F("flemme.ai"));
    display.display();

    delay(35);
  }

  delay(1800);
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

  if (Wire.available() < 6)
    return 0.0f;

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

// ================================================================
// SHAKE DETECTOR
// ================================================================

bool updateShake(float delta) {

  uint32_t now = millis();

  if ((now - lastShakeTrig) < SHAKE_COOLDOWN)
    return false;

  if (fabs(delta) < SHAKE_THRESHOLD)
    return false;

  if (shakePeakCount == 0)
    shakeStart = now;

  shakePeakCount++;

  if ((now - shakeStart) > SHAKE_WINDOW) {

    shakePeakCount = 1;
    shakeStart = now;
  }

  if (shakePeakCount >= SHAKE_MIN_PEAKS &&
      (now - shakeStart) >= 300) {

    shakePeakCount = 0;
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

void updateEncoderX()
{
    uint8_t state =
        (digitalRead(ENC_X_CLK) << 1) |
         digitalRead(ENC_X_DT);

    uint8_t transition =
        (prevXState << 2) | state;

    switch (transition)
    {
        case 0b0001:
        case 0b0111:
        case 0b1110:
        case 0b1000:
            xAccumulator++;
            lastDX = MOUSE_SPEED;
            lastDY = 0;
            break;

        case 0b0010:
        case 0b0100:
        case 0b1101:
        case 0b1011:
            xAccumulator--;
            lastDX = -MOUSE_SPEED;
            lastDY = 0;
            break;
    }

    if (xAccumulator>=4){Mouse.move(MOUSE_SPEED,0,0);lastDX=MOUSE_SPEED;lastDY=0;xAccumulator=0;}
if (xAccumulator<=-4){Mouse.move(-MOUSE_SPEED,0,0);lastDX=-MOUSE_SPEED;lastDY=0;xAccumulator=0;}
prevXState = state;
}

void updateEncoderY()
{
    uint8_t state =
        (digitalRead(ENC_Y_CLK) << 1) |
         digitalRead(ENC_Y_DT);

    uint8_t transition =
        (prevYState << 2) | state;

    switch (transition)
    {
        case 0b0001:
        case 0b0111:
        case 0b1110:
        case 0b1000:
            yAccumulator++;
            lastDX = 0;
            lastDY = MOUSE_SPEED;
            break;

        case 0b0010:
        case 0b0100:
        case 0b1101:
        case 0b1011:
            yAccumulator--;
            lastDX = 0;
            lastDY = -MOUSE_SPEED;
            break;
    }

    if (yAccumulator>=4){Mouse.move(0,MOUSE_SPEED,0);lastDX=0;lastDY=MOUSE_SPEED;yAccumulator=0;}
if (yAccumulator<=-4){Mouse.move(0,-MOUSE_SPEED,0);lastDX=0;lastDY=-MOUSE_SPEED;yAccumulator=0;}
prevYState = state;
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

  showStartupScreen();

  Mouse.begin();
  Mouse.release(MOUSE_LEFT);
  Keyboard.begin();

  randomSeed(micros());

  nextBlink = millis() + random(2000, 6000);

  prevXState =
      (digitalRead(ENC_X_CLK) << 1) |
      digitalRead(ENC_X_DT);

  prevYState =
      (digitalRead(ENC_Y_CLK) << 1) |
      digitalRead(ENC_Y_DT);
}

// ================================================================
// LOOP
// ================================================================

void loop() 
{
  // ------------------------------------------------
  // X Y ENCODERS
  // ------------------------------------------------
  updateEncoderX();
  updateEncoderY();

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
      blinking = false;
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
  if (millis() - lastDisplayRefresh >= DISPLAY_FPS_MS) {

    lastDisplayRefresh = millis();

    updateEyes();

    if (dizzyFace) {

      dizzyFrame++;
      drawDizzyFace();

      if (millis() > dizzyUntil)
        dizzyFace = false;

    } else {

      drawNormalFace();
    }
  }
}