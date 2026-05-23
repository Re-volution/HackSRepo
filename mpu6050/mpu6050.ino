/*
 * 项目：ESP32-C3 摔倒报警器（三段状态机版 + AP配网 + 心跳）
 * 功能：检测自由落体+撞击+长时间静止，避免误触发
 *       支持按键开启AP模式，通过网页配置WiFi
 *       每20小时发送一次心跳到服务器
 * 硬件：ESP32-C3 + MPU6050 + 蜂鸣器
 * I2C：SDA→GPIO4, SCL→GPIO5
 * 蜂鸣器：GPIO10
 */

#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <EEPROM.h>

// ========== 配网相关 ==========
#define EEPROM_SIZE 512
#define WIFI_SSID_ADDR 0
#define WIFI_PWD_ADDR  64

const int buttonPin = 9;           // 按键引脚（GPIO9）
bool apModeActive = false;
unsigned long apStartTime = 0;
const unsigned long AP_TIMEOUT = 120000;  // AP模式2分钟后自动关闭

WebServer server(80);

// 默认WiFi配置
const char* default_ssid = "JOJO990834";
const char* default_password = "xiaoming88";

String current_ssid = "";
String current_password = "";

// ========== 服务器配置 ==========
const char* serverUrl = "http://192.168.137.1:8080/api";
const unsigned long COOLDOWN_SECONDS = 10;

// ========== 心跳配置 ==========
const unsigned long HEARTBEAT_INTERVAL = 20 * 60 * 60 * 1000;  // 20小时
unsigned long lastHeartbeat = 0;

// ========== MPU6050 寄存器地址 ==========
const int MPU_ADDR = 0x68;
const int ACCEL_XOUT_H = 0x3B;
const int PWR_MGMT_1 = 0x6B;

// ========== 状态机变量 ==========
enum FallState {
  IDLE, FREEFALL, IMPACT, STILL_WAIT
};

const long ACTIVITY_AMP_THRESHOLD = 8500;
const int ACTIVITY_CONFIRM_COUNT = 8;

long baseAx, baseAy, baseAz;
bool baseReady = false;
int activityCounter = 0;

FallState fallState = IDLE;
unsigned long freefallStartTime = 0;
unsigned long impactTime = 0;
unsigned long stillStartTime = 0;
bool hasSent = false;
unsigned long lastSentTime = 0;

const long FREEFALL_THRESHOLD = 4000;
const long IMPACT_THRESHOLD = 30000;
const long STILL_MAX_DEVIATION = 8500;
const unsigned long STILL_REQUIRED_MS = 5000;

int16_t ax, ay, az;
long accelMagnitude;
const int ledPin = 8;
const int buzzerPin = 10;

long accelSamples[3][10];
int sampleIndex = 0;
bool samplesReady = false;
const String uuid = "老王家";

// ========== EEPROM 存储函数 ==========
void saveWiFiConfig(String ssid, String password) {
  EEPROM.begin(EEPROM_SIZE);
  
  for (int i = 0; i < ssid.length(); i++) {
    EEPROM.write(WIFI_SSID_ADDR + i, ssid[i]);
  }
  EEPROM.write(WIFI_SSID_ADDR + ssid.length(), '\0');
  
  for (int i = 0; i < password.length(); i++) {
    EEPROM.write(WIFI_PWD_ADDR + i, password[i]);
  }
  EEPROM.write(WIFI_PWD_ADDR + password.length(), '\0');
  
  EEPROM.commit();
  Serial.println("WiFi配置已保存");
}

void loadWiFiConfig() {
  EEPROM.begin(EEPROM_SIZE);
  
  String ssid = "";
  String pwd = "";
  
  for (int i = 0; i < 64; i++) {
    char c = EEPROM.read(WIFI_SSID_ADDR + i);
    if (c == '\0') break;
    if (c != 0xFF) ssid += c;
  }
  
  for (int i = 0; i < 64; i++) {
    char c = EEPROM.read(WIFI_PWD_ADDR + i);
    if (c == '\0') break;
    if (c != 0xFF) pwd += c;
  }
  
  if (ssid.length() > 0) {
    current_ssid = ssid;
    current_password = pwd;
    Serial.print("读取到保存的WiFi: ");
    Serial.println(current_ssid);
  } else {
    current_ssid = String(default_ssid);
    current_password = String(default_password);
    Serial.println("使用默认WiFi配置");
  }
}

// ========== 连接WiFi ==========
bool connectWiFi() {
  Serial.print("连接 WiFi: ");
  Serial.println(current_ssid);
  
  WiFi.begin(current_ssid.c_str(), current_password.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi 已连接");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    return true;
  } else {
    Serial.println("\nWiFi 连接失败");
    return false;
  }
}

// ========== 测试WiFi连接 ==========
bool testWiFiConnection(String ssid, String password) {
  Serial.print("测试连接 WiFi: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid.c_str(), password.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n连接成功！");
    WiFi.disconnect(true);
    delay(100);
    return true;
  } else {
    Serial.println("\n连接失败！");
    return false;
  }
}

// ========== LED 闪烁 ==========
void flashLED(int times, int durationMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(ledPin, HIGH);
    delay(durationMs);
    digitalWrite(ledPin, LOW);
    delay(durationMs);
  }
}

// ========== 蜂鸣器鸣叫 ==========
void beep(int times, int durationMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(buzzerPin, LOW);
    delay(durationMs);
    digitalWrite(buzzerPin, HIGH);
    delay(durationMs);
  }
}

// ========== 发送心跳 ==========
void sendHeartbeat() {
  Serial.println("发送心跳...");
  
  WiFi.begin(current_ssid.c_str(), current_password.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 15) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi 连接失败，心跳发送失败");
    return;
  }
  
  Serial.println("\nWiFi 已连接");
  
  HTTPClient http;
  http.begin(String(serverUrl) + "/heartbeat");
  http.addHeader("Content-Type", "application/json");
  
  String jsonData = "{\"device\":\"" + uuid + "\",\"type\":\"heartbeat\",\"timestamp\":";
  jsonData += millis();
  jsonData += "}";
  
  Serial.print("发送心跳: ");
  Serial.println(jsonData);
  
  int httpCode = http.POST(jsonData);
  if (httpCode > 0) {
    Serial.print("心跳响应码: ");
    Serial.println(httpCode);
  } else {
    Serial.print("心跳发送失败: ");
    Serial.println(http.errorToString(httpCode));
  }
  
  http.end();
  WiFi.disconnect(true);
  Serial.println("心跳发送完成，WiFi 已断开");
}

// ========== 发送报警 ==========
void sendAlert() {
  Serial.print("连接 WiFi: ");
  Serial.println(current_ssid);
  
  WiFi.begin(current_ssid.c_str(), current_password.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi 连接失败！");
    return;
  }
  
  Serial.println("\nWiFi 已连接");
  
  HTTPClient http;
  http.begin(String(serverUrl) + "/drop");
  http.addHeader("Content-Type", "application/json");
  
  String jsonData = "{\"device\":\"" + uuid + "\",\"event\":\"跌落\",\"timestamp\":";
  jsonData += millis();
  jsonData += ",\"accel\":[";
  jsonData += ax;
  jsonData += ",";
  jsonData += ay;
  jsonData += ",";
  jsonData += az;
  jsonData += "]}";
  
  Serial.print("发送数据: ");
  Serial.println(jsonData);
  
  int httpCode = http.POST(jsonData);
  if (httpCode > 0) {
    Serial.print("HTTP 响应码: ");
    Serial.println(httpCode);
    String response = http.getString();
    Serial.println("服务器响应: " + response);
    
    // 报警成功：蜂鸣器鸣叫5次
    beep(5, 300);
  } else {
    Serial.print("HTTP 请求失败: ");
    Serial.println(http.errorToString(httpCode));
  }
  
  http.end();
  WiFi.disconnect(true);
  Serial.println("WiFi 断开完成");
}

// ========== 深度睡眠 ==========
void goToDeepSleep() {
  Serial.println("进入深度睡眠，功耗降至约 5μA...");
  digitalWrite(ledPin, LOW);
  digitalWrite(buzzerPin, HIGH);
  esp_sleep_enable_timer_wakeup(60 * 1000000ULL);
  esp_deep_sleep_start();
}

// ========== AP模式网页 ==========
void handleRoot() {
  String html = "<!DOCTYPE html><html>";
  html += "<head><meta charset='UTF-8'><meta name='viewport' content='width=device-width'>";
  html += "<title>摔倒报警器配网</title>";
  html += "<style>";
  html += "body{font-family:Arial;text-align:center;margin-top:50px;background:#1a1a2e;color:#eee;}";
  html += ".container{background:#16213e;padding:30px;border-radius:15px;max-width:400px;margin:auto;}";
  html += "input{width:100%;padding:12px;margin:10px 0;border:none;border-radius:8px;}";
  html += "button{background:#e94560;color:white;padding:12px 24px;border:none;border-radius:8px;cursor:pointer;font-size:16px;}";
  html += "</style></head>";
  html += "<body><div class='container'>";
  html += "<h2>⚠️ 摔倒报警器配网</h2>";
  html += "<form action='/save' method='POST'>";
  html += "<input type='text' name='ssid' placeholder='WiFi名称' required>";
  html += "<input type='password' name='password' placeholder='WiFi密码'>";
  html += "<button type='submit'>保存并测试</button>";
  html += "</form>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "Missing ssid");
    return;
  }
  
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  
  Serial.print("收到WiFi配置 - SSID: ");
  Serial.println(ssid);
  
  if (testWiFiConnection(ssid, password)) {
    flashLED(1, 200);
    saveWiFiConfig(ssid, password);
    
    String html = "<!DOCTYPE html><html>";
    html += "<head><meta charset='UTF-8'><meta http-equiv='refresh' content='3;url=/'>";
    html += "<title>保存成功</title>";
    html += "<style>body{font-family:Arial;text-align:center;margin-top:50px;}</style>";
    html += "</head><body>";
    html += "<h2>✅ WiFi连接成功！</h2>";
    html += "<p>设备将在3秒后重启...</p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
    
    delay(3000);
    ESP.restart();
  } else {
    flashLED(5, 150);
    
    String html = "<!DOCTYPE html><html>";
    html += "<head><meta charset='UTF-8'>";
    html += "<title>连接失败</title>";
    html += "<style>body{font-family:Arial;text-align:center;margin-top:50px;}</style>";
    html += "</head><body>";
    html += "<h2>❌ WiFi连接失败！</h2>";
    html += "<button onclick='history.back()'>返回重试</button>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  }
}

void startAPMode() {
  apModeActive = true;
  apStartTime = millis();
  
  WiFi.disconnect(true);
  delay(100);
  
  WiFi.softAP("FallDetector_AP", "12345678");
  
  Serial.println("=== AP模式已开启 ===");
  Serial.println("热点: FallDetector_AP 密码: 12345678");
  
  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.begin();
  
  beep(3, 150);
  flashLED(5, 100);
}

void stopAPMode() {
  server.stop();
  WiFi.softAPdisconnect(true);
  apModeActive = false;
  Serial.println("=== AP模式已关闭 ===");
}

void checkButton() {
  static int lastState = HIGH;
  static unsigned long lastPressTime = 0;
  static int pressCount = 0;
  
  int currentState = digitalRead(buttonPin);
  
  if (lastState == HIGH && currentState == LOW) {
    unsigned long now = millis();
    if (now - lastPressTime < 500) {
      pressCount++;
    } else {
      pressCount = 1;
    }
    lastPressTime = now;
    Serial.print("按键按下: ");
    Serial.println(pressCount);
  }
  lastState = currentState;
  
  if (pressCount == 3 && !apModeActive) {
    pressCount = 0;
    startAPMode();
  }
}

// ========== MPU6050 函数 ==========
bool initMPU6050() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(PWR_MGMT_1);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(100);
  return true;
}

void readAccel() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(ACCEL_XOUT_H);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true);
  if (Wire.available() == 6) {
    ax = (Wire.read() << 8) | Wire.read();
    ay = (Wire.read() << 8) | Wire.read();
    az = (Wire.read() << 8) | Wire.read();
  }
  accelMagnitude = sqrt((long)ax * ax + (long)ay * ay + (long)az * az);
}

void recordAccelSample() {
  accelSamples[0][sampleIndex] = ax;
  accelSamples[1][sampleIndex] = ay;
  accelSamples[2][sampleIndex] = az;
  sampleIndex = (sampleIndex + 1) % 10;
  if (sampleIndex == 0) samplesReady = true;
}

bool isStill() {
  if (!samplesReady) return false;
  long minX = 32767, maxX = -32768;
  long minY = 32767, maxY = -32768;
  long minZ = 32767, maxZ = -32768;
  for (int i = 0; i < 10; i++) {
    if (accelSamples[0][i] < minX) minX = accelSamples[0][i];
    if (accelSamples[0][i] > maxX) maxX = accelSamples[0][i];
    if (accelSamples[1][i] < minY) minY = accelSamples[1][i];
    if (accelSamples[1][i] > maxY) maxY = accelSamples[1][i];
    if (accelSamples[2][i] < minZ) minZ = accelSamples[2][i];
    if (accelSamples[2][i] > maxZ) maxZ = accelSamples[2][i];
  }
  return (maxX - minX) < STILL_MAX_DEVIATION &&
         (maxY - minY) < STILL_MAX_DEVIATION &&
         (maxZ - minZ) < STILL_MAX_DEVIATION;
}

// ========== 主程序 ==========
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(ledPin, OUTPUT);
  pinMode(buzzerPin, OUTPUT);
  pinMode(buttonPin, INPUT_PULLUP);
  digitalWrite(buzzerPin, HIGH);
  digitalWrite(ledPin, LOW);
  
  for (int i = 0; i < 3; i++) {
    digitalWrite(ledPin, HIGH);
    delay(100);
    digitalWrite(ledPin, LOW);
    delay(100);
  }
  
  Serial.println("=== 摔倒报警器启动（三段状态机 + AP配网 + 心跳）===");
  
  loadWiFiConfig();
  
  if (!connectWiFi()) {
    Serial.println("WiFi连接失败，按3次按键开启AP配网模式");
  }else{
    sendHeartbeat();
    lastHeartbeat = millis();
  }
  
  Wire.begin(4, 5);
  Serial.println("I2C 初始化完成");
  
  if (!initMPU6050()) {
    Serial.println("错误：MPU6050 初始化失败！请检查接线");
    while (1) {
      digitalWrite(ledPin, HIGH);
      delay(500);
      digitalWrite(ledPin, LOW);
      delay(500);
    }
  }
  Serial.println("MPU6050 初始化成功");
  
  for (int i = 0; i < 10; i++) {
    readAccel();
    recordAccelSample();
    delay(50);
  }
  
  lastHeartbeat = millis();
  
  Serial.println("开始监控，等待摔倒事件...");
  Serial.println("提示：连续按3次按键可开启AP配网模式");
}

void loop() {
  checkButton();
  
  if (apModeActive) {
    server.handleClient();
    if (millis() - apStartTime > AP_TIMEOUT) {
      stopAPMode();
    }
    delay(10);
    return;
  }
  
  // 心跳检测
  if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
    lastHeartbeat = millis();
    sendHeartbeat();
  }
  
  readAccel();
  recordAccelSample();
  
  long accelSum = abs(ax) + abs(ay) + abs(az);
  
  if (accelMagnitude > 40000 || accelSum < 8000) {
    Serial.print("Sum="); Serial.print(accelSum);
    Serial.print(" Mag="); Serial.print(accelMagnitude);
    Serial.print(" State="); Serial.println(fallState);
  }
  
  switch (fallState) {
    case IDLE:
      if (accelSum < FREEFALL_THRESHOLD) {
        fallState = FREEFALL;
        freefallStartTime = millis();
        Serial.println("-> FREEFALL");
      }
      break;
      
    case FREEFALL:
      if (accelMagnitude > IMPACT_THRESHOLD) {
        if (millis() - freefallStartTime < 500) {
          fallState = IMPACT;
          impactTime = millis();
          sampleIndex = 0;
          samplesReady = false;
          Serial.println("-> IMPACT");
        } else {
          fallState = IDLE;
          Serial.println("自由落体超时，回到IDLE");
        }
      } else if (millis() - freefallStartTime > 1000) {
        fallState = IDLE;
        Serial.println("自由落体后无撞击，回到IDLE");
      }
      break;
      
    case IMPACT:
      if (millis() - impactTime > 500) {
        fallState = STILL_WAIT;
        stillStartTime = millis();
        baseAx = ax;
        baseAy = ay;
        baseAz = az;
        baseReady = true;
        activityCounter = 0;
        Serial.println("-> STILL_WAIT (基准已记录)");
      }
      break;
      
    case STILL_WAIT: {
      if (!baseReady) {
        baseAx = ax; baseAy = ay; baseAz = az;
        baseReady = true;
      }
      
      long diffX = abs(ax - baseAx);
      long diffY = abs(ay - baseAy);
      long diffZ = abs(az - baseAz);
      
      bool significantMove = (diffX > ACTIVITY_AMP_THRESHOLD ||
                              diffY > ACTIVITY_AMP_THRESHOLD ||
                              diffZ > ACTIVITY_AMP_THRESHOLD);
      
      if (!significantMove) {
        unsigned long stillDuration = millis() - stillStartTime;
        if (stillDuration >= STILL_REQUIRED_MS) {
          Serial.println("⚠️ 确认摔倒且长时间静止！触发报警");
          if (!hasSent && (millis() - lastSentTime > COOLDOWN_SECONDS * 1000)) {
            for (int i = 0; i < 5; i++) {
              digitalWrite(ledPin, HIGH);
              delay(50);
              digitalWrite(ledPin, LOW);
              delay(50);
            }
            sendAlert();
            lastSentTime = millis();
            hasSent = true;
            Serial.println("报警已发送，5秒后深度睡眠...");
            goToDeepSleep();
          }
          fallState = IDLE;
        }
      } else {
        activityCounter++;
        if (activityCounter >= ACTIVITY_CONFIRM_COUNT) {
          Serial.println("检测到明显活动，取消报警");
          fallState = IDLE;
          activityCounter = 0;
        }
      }
      break;
    }
  }
  
  delay(20);
}