/*
 * ==============================================================================
 *  ESP32 Wi-Fi Mobile Remote Control Car (Pure Motor Drive Edition)
 * ==============================================================================
 *  Hardware Connections:
 *  -----------------------------------------------------------------------------
 *  L298N Motor Driver to ESP32:
 *    - ENA -> GPIO 25 (Left Motors PWM Speed - Remove jumper cap!)
 *    - IN1 -> GPIO 26 (Left Motors Direction 1)
 *    - IN2 -> GPIO 27 (Left Motors Direction 2)
 *    - IN3 -> GPIO 14 (Right Motors Direction 1)
 *    - IN4 -> GPIO 18 (Right Motors Direction 2)
 *    - ENB -> GPIO 33 (Right Motors PWM Speed - Remove jumper cap!)
 *
 *  Power Connections:
 *    - Battery (+)  -> L298N 12V terminal
 *    - Battery (-)  -> L298N GND terminal AND ESP32 GND (Common Ground)
 *    - L298N 5V Out -> ESP32 VIN
 * ==============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>

// ======================== PIN ASSIGNMENTS ========================
// L298N Motor Driver Pins
#define PIN_ENA   25  // Left Motors PWM Speed
#define PIN_IN1   26  // Left Motors Direction 1
#define PIN_IN2   27  // Left Motors Direction 2
#define PIN_IN3   14  // Right Motors Direction 1
#define PIN_IN4   18  // Right Motors Direction 2
#define PIN_ENB   33  // Right Motors PWM Speed

// Optional Onboard LED for status indication
#define PIN_LED    2

// ======================== WI-FI CONFIGURATION ====================
// ESP32 broadcasts its own Wi-Fi Hotspot (Access Point mode)
const char* AP_SSID     = "ESP32-RC-CAR";
const char* AP_PASSWORD = "password123";  // Min 8 characters

const byte DNS_PORT = 53;
DNSServer dnsServer;
WebServer server(80);

IPAddress apIP(192, 168, 4, 1);

// ======================== DRIVING & CALIBRATION ==================
int currentSpeed = 190;           // Calibrated default PWM speed (0 - 255)
int steeringTrim = 0;             // Straight-line balance (-50 to +50)
String currentDirection = "S";    // S = Stop, F = Forward, B = Backward, etc.
const int MIN_MOTOR_PWM = 95;     // Minimum torque floor to prevent stall hum

// Motor Direction Inversion switches (true = reversed)
const bool INVERT_LEFT_MOTORS  = true;  // Calibrated for forward drive
const bool INVERT_RIGHT_MOTORS = true;  // Calibrated for forward drive

// Safety Watchdog: Stops motors if communication drops
unsigned long lastCommandTime = 0;
const unsigned long FAILSAFE_TIMEOUT_MS = 2500; // Stop after 2.5s without heartbeats

// ======================== PWM MOTOR SPEED CONTROL ================
// Compatible with both ESP32 Arduino Core 2.x and 3.x
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  // ESP32 Core 3.x API
  void initPWM() {
    ledcAttach(PIN_ENA, 1000, 8);   // 1kHz, 8-bit (0-255)
    ledcAttach(PIN_ENB, 1000, 8);   // 1kHz, 8-bit (0-255)
  }

  void setLeftSpeed(uint8_t speed) {
    ledcWrite(PIN_ENA, speed);
  }

  void setRightSpeed(uint8_t speed) {
    ledcWrite(PIN_ENB, speed);
  }

#else
  // ESP32 Core 2.x API
  #define CH_ENA 0
  #define CH_ENB 1

  void initPWM() {
    ledcSetup(CH_ENA, 1000, 8);
    ledcAttachPin(PIN_ENA, CH_ENA);

    ledcSetup(CH_ENB, 1000, 8);
    ledcAttachPin(PIN_ENB, CH_ENB);
  }

  void setLeftSpeed(uint8_t speed) {
    ledcWrite(CH_ENA, speed);
  }

  void setRightSpeed(uint8_t speed) {
    ledcWrite(CH_ENB, speed);
  }
#endif

// ======================== MOTOR MOVEMENT LOGIC ===================
void setLeftMotorState(int state, int spd) {
  // state: 1 = Forward, -1 = Backward, 0 = Stop
  if (INVERT_LEFT_MOTORS) state = -state;

  if (state == 1) {
    digitalWrite(PIN_IN1, HIGH);
    digitalWrite(PIN_IN2, LOW);
  } else if (state == -1) {
    digitalWrite(PIN_IN1, LOW);
    digitalWrite(PIN_IN2, HIGH);
  } else {
    digitalWrite(PIN_IN1, LOW);
    digitalWrite(PIN_IN2, LOW);
    spd = 0;
  }
  setLeftSpeed(spd);
}

void setRightMotorState(int state, int spd) {
  // state: 1 = Forward, -1 = Backward, 0 = Stop
  if (INVERT_RIGHT_MOTORS) state = -state;

  if (state == 1) {
    digitalWrite(PIN_IN3, HIGH);
    digitalWrite(PIN_IN4, LOW);
  } else if (state == -1) {
    digitalWrite(PIN_IN3, LOW);
    digitalWrite(PIN_IN4, HIGH);
  } else {
    digitalWrite(PIN_IN3, LOW);
    digitalWrite(PIN_IN4, LOW);
    spd = 0;
  }
  setRightSpeed(spd);
}

void stopMotors() {
  currentDirection = "S";
  // Active Electromagnetic Dynamic Braking:
  // Short motor coils to stop wheel inertia instantly without rolling/coasting!
  digitalWrite(PIN_IN1, HIGH);
  digitalWrite(PIN_IN2, HIGH);
  digitalWrite(PIN_IN3, HIGH);
  digitalWrite(PIN_IN4, HIGH);
  setLeftSpeed(255);
  setRightSpeed(255);
  delay(35); // 35ms dynamic brake pulse

  // Return to safe neutral idle
  setLeftMotorState(0, 0);
  setRightMotorState(0, 0);
}

void driveMotors(String dir, int spd) {
  spd = constrain(spd, 0, 255);
  currentSpeed = spd;
  currentDirection = dir;
  lastCommandTime = millis();

  if (dir == "S") {
    stopMotors();
    return;
  }

  // Calculate trimmed left and right speeds for straight-line accuracy
  int leftPwm = spd;
  int rightPwm = spd;

  if (steeringTrim > 0) {
    // Car was drifting right -> reduce right motor slightly to balance
    rightPwm = map(steeringTrim, 0, 50, spd, (spd * 60) / 100);
  } else if (steeringTrim < 0) {
    // Car was drifting left -> reduce left motor slightly to balance
    leftPwm = map(-steeringTrim, 0, 50, spd, (spd * 60) / 100);
  }

  // Ensure speed is above the stall torque threshold
  leftPwm = max(leftPwm, MIN_MOTOR_PWM);
  rightPwm = max(rightPwm, MIN_MOTOR_PWM);

  // Controlled turn speeds: smooth and predictable (prevents violent 360 spinning)
  int pivotSpd = constrain((spd * 72) / 100, MIN_MOTOR_PWM, 200);
  int arcInner = constrain((spd * 48) / 100, MIN_MOTOR_PWM - 5, 170); // Non-stalling inner wheel
  int arcOuter = spd;

  if (dir == "F") {
    // Accurate straight-line forward with trim balance
    setLeftMotorState(1, leftPwm);
    setRightMotorState(1, rightPwm);
  } 
  else if (dir == "B") {
    // Accurate straight-line reverse with trim balance
    setLeftMotorState(-1, leftPwm);
    setRightMotorState(-1, rightPwm);
  } 
  else if (dir == "L") {
    // Precision Pivot Left (Tank turn CCW at controlled rate)
    setLeftMotorState(-1, pivotSpd);
    setRightMotorState(1, pivotSpd);
  } 
  else if (dir == "R") {
    // Precision Pivot Right (Tank turn CW at controlled rate)
    setLeftMotorState(1, pivotSpd);
    setRightMotorState(-1, pivotSpd);
  } 
  else if (dir == "FL") {
    // Smooth Arc Forward-Left (Both wheels spin smoothly)
    setLeftMotorState(1, arcInner);
    setRightMotorState(1, arcOuter);
  } 
  else if (dir == "FR") {
    // Smooth Arc Forward-Right
    setLeftMotorState(1, arcOuter);
    setRightMotorState(1, arcInner);
  } 
  else if (dir == "BL") {
    // Smooth Arc Reverse-Left
    setLeftMotorState(-1, arcInner);
    setRightMotorState(-1, arcOuter);
  } 
  else if (dir == "BR") {
    // Smooth Arc Reverse-Right
    setLeftMotorState(-1, arcOuter);
    setRightMotorState(-1, arcInner);
  } 
  else {
    stopMotors();
  }
}

// ======================== HTML & MOBILE DASHBOARD ================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover">
  <title>ESP32 RC Car Cockpit</title>
  <style>
    :root {
      --bg: #0c0f17;
      --card-bg: rgba(23, 28, 44, 0.85);
      --card-border: rgba(255, 255, 255, 0.08);
      --accent-cyan: #00e5ff;
      --accent-orange: #ff9100;
      --accent-red: #ff1744;
      --accent-green: #00e676;
      --text-main: #f0f4f8;
      --text-dim: #8a99ad;
    }
    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      user-select: none;
      -webkit-user-select: none;
      touch-action: manipulation;
    }
    body {
      background-color: var(--bg);
      background-image: radial-gradient(circle at 50% 10%, #17223b 0%, #0c0f17 100%);
      color: var(--text-main);
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: 14px;
      overflow-x: hidden;
    }
    header {
      width: 100%;
      max-width: 440px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 12px 18px;
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 18px;
      backdrop-filter: blur(10px);
      margin-bottom: 14px;
      box-shadow: 0 8px 24px rgba(0,0,0,0.3);
    }
    .brand {
      display: flex;
      align-items: center;
      gap: 10px;
    }
    .brand-icon {
      font-size: 26px;
    }
    .brand-text h1 {
      font-size: 17px;
      font-weight: 800;
      letter-spacing: 0.5px;
      background: linear-gradient(90deg, var(--accent-cyan), #7c4dff);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
    }
    .brand-text span {
      font-size: 11px;
      color: var(--text-dim);
    }
    .status-badge {
      font-size: 11px;
      padding: 5px 12px;
      border-radius: 20px;
      background: rgba(0, 230, 118, 0.15);
      color: var(--accent-green);
      border: 1px solid rgba(0, 230, 118, 0.3);
      display: flex;
      align-items: center;
      gap: 6px;
      font-weight: 700;
    }
    .status-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: var(--accent-green);
      box-shadow: 0 0 8px var(--accent-green);
      animation: pulse 1.6s infinite;
    }
    @keyframes pulse {
      0% { opacity: 0.4; }
      50% { opacity: 1; }
      100% { opacity: 0.4; }
    }
    .dashboard-container {
      width: 100%;
      max-width: 440px;
      display: flex;
      flex-direction: column;
      gap: 12px;
    }
    .card {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 20px;
      padding: 16px;
      backdrop-filter: blur(8px);
      box-shadow: 0 6px 18px rgba(0,0,0,0.25);
    }
    .card-title {
      font-size: 12px;
      text-transform: uppercase;
      letter-spacing: 1px;
      color: var(--text-dim);
      margin-bottom: 10px;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }

    /* SLIDERS & CONTROLS */
    .slider-row {
      display: flex;
      align-items: center;
      gap: 12px;
      margin-top: 6px;
    }
    .custom-range {
      flex: 1;
      height: 8px;
      border-radius: 4px;
      background: #1b263b;
      outline: none;
      -webkit-appearance: none;
    }
    .custom-range::-webkit-slider-thumb {
      -webkit-appearance: none;
      appearance: none;
      width: 28px;
      height: 28px;
      border-radius: 50%;
      background: var(--accent-cyan);
      cursor: pointer;
      box-shadow: 0 0 10px var(--accent-cyan);
    }
    .slider-val {
      min-width: 48px;
      font-weight: 700;
      text-align: right;
      font-size: 15px;
      color: var(--accent-cyan);
    }
    .preset-pills {
      display: flex;
      gap: 8px;
      margin-top: 10px;
    }
    .pill {
      flex: 1;
      background: rgba(255,255,255,0.06);
      border: 1px solid rgba(255,255,255,0.1);
      padding: 8px 0;
      border-radius: 12px;
      font-size: 11px;
      font-weight: 700;
      color: var(--text-dim);
      text-align: center;
      cursor: pointer;
      transition: background-color 0.1s ease;
    }
    .pill:active {
      background: var(--accent-cyan);
      color: #000;
    }

    /* D-PAD JOYSTICK CONTROLS */
    .dpad-container {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      grid-template-rows: repeat(3, 85px);
      gap: 12px;
      justify-items: center;
      align-items: center;
      max-width: 320px;
      margin: 8px auto;
    }
    .d-btn {
      width: 100%;
      height: 100%;
      background: rgba(255, 255, 255, 0.05);
      border: 1px solid rgba(255, 255, 255, 0.12);
      border-radius: 18px;
      color: var(--text-main);
      font-size: 26px;
      display: flex;
      flex-direction: column;
      justify-content: center;
      align-items: center;
      gap: 4px;
      cursor: pointer;
      transition: transform 0.08s ease, background-color 0.08s ease, border-color 0.08s ease;
      box-shadow: 0 4px 12px rgba(0,0,0,0.2);
      outline: none;
      -webkit-tap-highlight-color: transparent;
    }
    .d-btn span {
      font-size: 10px;
      font-weight: 700;
      letter-spacing: 0.5px;
      color: var(--text-dim);
    }
    .d-btn:active, .d-btn.active {
      transform: scale(0.93);
      background: rgba(0, 229, 255, 0.25);
      border-color: var(--accent-cyan);
      box-shadow: 0 0 16px rgba(0, 229, 255, 0.4);
    }
    .btn-stop {
      background: rgba(255, 23, 68, 0.15);
      border-color: rgba(255, 23, 68, 0.35);
      color: var(--accent-red);
    }
    .btn-stop:active, .btn-stop.active {
      background: var(--accent-red);
      color: white;
      box-shadow: 0 0 20px rgba(255, 23, 68, 0.6);
    }
    footer {
      margin-top: 14px;
      text-align: center;
      font-size: 11px;
      color: var(--text-dim);
    }
  </style>
</head>
<body>

  <!-- HEADER -->
  <header>
    <div class="brand">
      <div class="brand-icon">🏎️</div>
      <div class="brand-text">
        <h1>ESP32 ROVER</h1>
        <span>Fast Wi-Fi Controller</span>
      </div>
    </div>
    <div class="status-badge">
      <div class="status-dot"></div>
      <span>CONNECTED</span>
    </div>
  </header>

  <div class="dashboard-container">

    <!-- SPEED SETTING -->
    <div class="card">
      <div class="card-title">
        <span>Motor Speed (Power)</span>
        <span id="speedPercent">75%</span>
      </div>
      <div class="slider-row">
        <span>🐢</span>
        <input type="range" min="100" max="255" value="190" class="custom-range" id="speedSlider" oninput="onSpeedChange(this.value)">
        <span>⚡</span>
        <div class="slider-val" id="speedVal">190</div>
      </div>
      <div class="preset-pills">
        <div class="pill" onclick="setSpeed(130)">PRECISE (50%)</div>
        <div class="pill" onclick="setSpeed(190)">CRUISE (75%)</div>
        <div class="pill" onclick="setSpeed(255)">BOOST (100%)</div>
      </div>
    </div>

    <!-- STEERING TRIM & STRAIGHT-LINE ALIGNMENT -->
    <div class="card">
      <div class="card-title">
        <span>Straight-Line Steering Trim</span>
        <span id="trimVal" style="color:var(--accent-cyan); font-weight:700;">0 (Dead Center)</span>
      </div>
      <div class="slider-row">
        <span>◀ L</span>
        <input type="range" min="-35" max="35" value="0" class="custom-range" id="trimSlider" oninput="onTrimChange(this.value)">
        <span>R ▶</span>
      </div>
      <div class="preset-pills">
        <div class="pill" onclick="adjustTrim(-3)">◀ Trim Left</div>
        <div class="pill" onclick="resetTrim()">RESET (0)</div>
        <div class="pill" onclick="adjustTrim(3)">Trim Right ▶</div>
      </div>
    </div>

    <!-- D-PAD CONTROLLER -->
    <div class="card">
      <div class="card-title">
        <span>Steering & Motion</span>
        <span id="dirState" style="color:var(--accent-orange); font-weight:700;">STOPPED</span>
      </div>
      <div class="dpad-container">
        <!-- Row 1: Forward-Left, Forward, Forward-Right -->
        <button class="d-btn" data-dir="FL"><span>↖</span></button>
        <button class="d-btn" data-dir="F">▲<span>FWD</span></button>
        <button class="d-btn" data-dir="FR"><span>↗</span></button>

        <!-- Row 2: Pivot Left, STOP, Pivot Right -->
        <button class="d-btn" data-dir="L">⟲<span>LEFT</span></button>
        <button class="d-btn btn-stop" data-dir="S">■<span>STOP</span></button>
        <button class="d-btn" data-dir="R">⟳<span>RIGHT</span></button>

        <!-- Row 3: Backward-Left, Backward, Backward-Right -->
        <button class="d-btn" data-dir="BL"><span>↙</span></button>
        <button class="d-btn" data-dir="B">▼<span>REV</span></button>
        <button class="d-btn" data-dir="BR"><span>↘</span></button>
      </div>
    </div>

  </div>

  <footer>ESP32 Wi-Fi Rover • IP: 192.168.4.1</footer>

  <script>
    let activeDir = 'S';
    let currentSpeed = 190;
    let currentTrim = 0;
    let driveAbort = null;

    // Send driving command to ESP32 with zero-latency abort
    function sendDriveCmd(dir) {
      if (activeDir === dir && dir !== 'S') return;
      activeDir = dir;
      updateDirLabel(dir);

      if (driveAbort) {
        driveAbort.abort();
      }
      driveAbort = new AbortController();

      fetch(`/drive?dir=${dir}&speed=${currentSpeed}&trim=${currentTrim}`, {
        signal: driveAbort.signal,
        cache: 'no-store'
      }).catch(err => {
        if (err.name !== 'AbortError') console.warn('Drive cmd failed:', err);
      });
    }

    // Steering Trim handlers
    function onTrimChange(val) {
      currentTrim = parseInt(val);
      const label = document.getElementById('trimVal');
      if (currentTrim > 0) label.innerText = `+${currentTrim} (Right Bias)`;
      else if (currentTrim < 0) label.innerText = `${currentTrim} (Left Bias)`;
      else label.innerText = '0 (Dead Center)';

      fetch(`/trim?val=${currentTrim}`);
    }

    function adjustTrim(delta) {
      let val = parseInt(document.getElementById('trimSlider').value) + delta;
      val = Math.max(-35, Math.min(35, val));
      document.getElementById('trimSlider').value = val;
      onTrimChange(val);
    }

    function resetTrim() {
      document.getElementById('trimSlider').value = 0;
      onTrimChange(0);
    }

    function updateDirLabel(dir) {
      const labels = {
        'S': 'STOPPED ■',
        'F': 'FORWARD ▲',
        'B': 'REVERSE ▼',
        'L': 'PIVOT LEFT ⟲',
        'R': 'PIVOT RIGHT ⟳',
        'FL': 'FWD-LEFT ↖',
        'FR': 'FWD-RIGHT ↗',
        'BL': 'REV-LEFT ↙',
        'BR': 'REV-RIGHT ↘'
      };
      document.getElementById('dirState').innerText = labels[dir] || dir;
    }

    // Touch & Mouse bindings for responsive hold-to-drive
    const buttons = document.querySelectorAll('.d-btn');
    buttons.forEach(btn => {
      const dir = btn.getAttribute('data-dir');

      const startAction = (e) => {
        e.preventDefault();
        btn.classList.add('active');
        sendDriveCmd(dir);
      };

      const endAction = (e) => {
        e.preventDefault();
        btn.classList.remove('active');
        if (dir !== 'S') {
          sendDriveCmd('S');
        }
      };

      btn.addEventListener('touchstart', startAction, { passive: false });
      btn.addEventListener('touchend', endAction, { passive: false });
      btn.addEventListener('touchcancel', endAction, { passive: false });
      btn.addEventListener('mousedown', startAction);
      btn.addEventListener('mouseup', endAction);
      btn.addEventListener('mouseleave', () => {
        if (btn.classList.contains('active') && dir !== 'S') {
          btn.classList.remove('active');
          sendDriveCmd('S');
        }
      });
    });

    // Keyboard support for desktop testing (WASD / Arrows)
    const keyMap = {
      'ArrowUp': 'F', 'KeyW': 'F',
      'ArrowDown': 'B', 'KeyS': 'B',
      'ArrowLeft': 'L', 'KeyA': 'L',
      'ArrowRight': 'R', 'KeyD': 'R',
      'Space': 'S'
    };
    let keyPressed = {};
    window.addEventListener('keydown', (e) => {
      if (keyMap[e.code] && !keyPressed[e.code]) {
        keyPressed[e.code] = true;
        sendDriveCmd(keyMap[e.code]);
      }
    });
    window.addEventListener('keyup', (e) => {
      if (keyMap[e.code]) {
        keyPressed[e.code] = false;
        sendDriveCmd('S');
      }
    });

    // Speed Slider
    function onSpeedChange(val) {
      currentSpeed = val;
      document.getElementById('speedVal').innerText = val;
      document.getElementById('speedPercent').innerText = Math.round((val / 255) * 100) + '%';
      if (activeDir !== 'S') {
        sendDriveCmd(activeDir);
      }
    }
    function setSpeed(val) {
      document.getElementById('speedSlider').value = val;
      onSpeedChange(val);
    }
  </script>
</body>
</html>
)rawliteral";

// ======================== HTTP ENDPOINTS =========================
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleDrive() {
  String dir = "S";
  int spd = currentSpeed;

  if (server.hasArg("dir")) {
    dir = server.arg("dir");
  }
  if (server.hasArg("speed")) {
    spd = server.arg("speed").toInt();
  }
  if (server.hasArg("trim")) {
    steeringTrim = constrain(server.arg("trim").toInt(), -50, 50);
  }

  driveMotors(dir, spd);

  String json = "{\"status\":\"ok\",\"dir\":\"" + dir + "\",\"speed\":" + String(spd) + ",\"trim\":" + String(steeringTrim) + "}";
  server.send(200, "application/json", json);
}

void handleTrim() {
  if (server.hasArg("val")) {
    steeringTrim = constrain(server.arg("val").toInt(), -50, 50);
    server.send(200, "application/json", "{\"status\":\"ok\",\"trim\":" + String(steeringTrim) + "}");
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Missing val\"}");
  }
}

// ======================== SETUP & MAIN LOOP ======================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n========================================");
  Serial.println("     ESP32 RC CAR (MOTOR EDITION)       ");
  Serial.println("========================================");

  // Configure Motor Direction Pins as OUTPUT
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_IN3, OUTPUT);
  pinMode(PIN_IN4, OUTPUT);

  // Onboard LED
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  // Initialize PWM for Motors (ENA, ENB)
  initPWM();
  stopMotors();

  // Initialize Wi-Fi Access Point (Hotspot)
  WiFi.mode(WIFI_AP);
  bool apSuccess = WiFi.softAP(AP_SSID, AP_PASSWORD);

  if (apSuccess) {
    Serial.println("[Wi-Fi] SoftAP started successfully!");
    Serial.print("[Wi-Fi] Network SSID : ");
    Serial.println(AP_SSID);
    Serial.print("[Wi-Fi] Password     : ");
    Serial.println(AP_PASSWORD);
    Serial.print("[Wi-Fi] IP Address   : http://");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("[Wi-Fi] ERROR: Failed to start SoftAP!");
  }

  // Set up mDNS (http://esp32car.local)
  if (MDNS.begin("esp32car")) {
    Serial.println("[mDNS] Responder started: http://esp32car.local");
  }

  // Start Captive Portal DNS Server (redirects all queries to ESP32 IP)
  dnsServer.start(DNS_PORT, "*", apIP);
  Serial.println("[DNS] Captive Portal DNS Server active!");

  // Register Web Server Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/drive", HTTP_GET, handleDrive);
  server.on("/trim", HTTP_GET, handleTrim);

  // Captive Portal probes for Android, iOS, Chrome
  server.on("/generate_204", HTTP_GET, handleRoot);
  server.on("/gen_204", HTTP_GET, handleRoot);
  server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
  server.on("/ncsi.txt", HTTP_GET, handleRoot);
  server.onNotFound(handleRoot); // Redirect ANY unknown URL to cockpit!

  server.begin();
  Serial.println("[Web Server] HTTP server online on port 80!");
  Serial.println("Ready to connect from phone!");
}

void loop() {
  // Process Captive Portal DNS queries
  dnsServer.processNextRequest();

  // Handle client HTTP requests
  server.handleClient();

  // Safety Watchdog: If car is moving and no command received for FAILSAFE_TIMEOUT_MS, stop!
  if (currentDirection != "S" && (millis() - lastCommandTime > FAILSAFE_TIMEOUT_MS)) {
    Serial.println("[FAILSAFE] Signal timeout! Emergency Stop activated.");
    stopMotors();
  }

  delay(2); // Yield to FreeRTOS
}
