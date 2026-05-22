/*
 * 项目：ESP32-C3 摔倒报警器（三段状态机版）
 * 功能：检测自由落体+撞击+长时间静止，避免误触发
 * 硬件：ESP32-C3 + MPU6050
 * I2C：SDA→GPIO4, SCL→GPIO5
 */

#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ========== 网络配置 ==========
const char* ssid     = "JOJO990834";
const char* password = "xiaoming88";
const char* serverUrl = "http://192.168.137.1:8080/api/drop";

const unsigned long COOLDOWN_SECONDS = 10;    // 防重复触发间隔（秒）

// ========== MPU6050 寄存器地址 ==========
const int MPU_ADDR = 0x68;
const int ACCEL_XOUT_H = 0x3B;
const int PWR_MGMT_1 = 0x6B;

// ========== 状态机变量 ==========
enum FallState {
  IDLE,           // 空闲
  FREEFALL,       // 检测到自由落体
  IMPACT,         // 检测到撞击
  STILL_WAIT      // 等待静止确认
};
// 活动检测参数
const long ACTIVITY_AMP_THRESHOLD = 8500;   // 单轴偏离基准超过此值才算“明显活动”
const int ACTIVITY_CONFIRM_COUNT = 8;        // 需要连续多次明显活动才取消

// 基准值变量
long baseAx, baseAy, baseAz;
bool baseReady = false;

// 活动计数器
int activityCounter = 0;

FallState fallState = IDLE;
unsigned long freefallStartTime = 0;
unsigned long impactTime = 0;
unsigned long stillStartTime = 0;
bool hasSent = false;
unsigned long lastSentTime = 0;

// ========== 阈值参数（可根据串口数据调整）==========
const long FREEFALL_THRESHOLD = 4000;      // accelSum < 4000 认为失重
const long IMPACT_THRESHOLD = 30000;       // accelMagnitude > 30000 认为撞击
const long STILL_MAX_DEVIATION = 8500;     // 静止时三轴变化范围（峰值-谷值）
const unsigned long STILL_REQUIRED_MS = 5000;   // 需要保持静止5秒


// ========== 全局变量 ==========
int16_t ax, ay, az;
long accelMagnitude;           // 合成加速度强度
const int ledPin = 8;          // ESP32-C3 板载 LED

// 用于静止检测的滑动窗口（10个样本）
long accelSamples[3][10];
int sampleIndex = 0; 
bool samplesReady = false;
// 唯一id 
const String uuid = "老王家";//这里可以换成唯一id做映射防止重名之类的

// ========== MPU6050 初始化 ==========
bool initMPU6050() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(PWR_MGMT_1);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) {
    return false;
  }
  delay(100);
  return true;
}

// ========== 读取加速度数据 ==========
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

// ========== 记录加速度样本（用于静止检测）==========
void recordAccelSample() {
  accelSamples[0][sampleIndex] = ax;
  accelSamples[1][sampleIndex] = ay;
  accelSamples[2][sampleIndex] = az;
  sampleIndex = (sampleIndex + 1) % 10;
  if (sampleIndex == 0) samplesReady = true;
}

// ========== 检测最近10个样本是否静止 ==========
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
  
  long rangeX = maxX - minX;
  long rangeY = maxY - minY;
  long rangeZ = maxZ - minZ;
  
  return (rangeX < STILL_MAX_DEVIATION &&
          rangeY < STILL_MAX_DEVIATION &&
          rangeZ < STILL_MAX_DEVIATION);
}

// ========== 发送报警到服务器 ==========
void sendAlert() {
  Serial.print("连接 WiFi: ");
  Serial.println(ssid);
  
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  const int maxRetries = 15;
  const int retryDelay = 500;

  while (WiFi.status() != WL_CONNECTED && attempts < maxRetries) {
    delay(retryDelay);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi 连接失败！");
    return;
  }
  
  Serial.println("\nWiFi 已连接");
  
  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");
  
  String jsonData = "{\"device\":\"" + String(uuid) + "\",\"event\":\"跌落\",\"timestamp\":";
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
  } else {
    Serial.print("HTTP 请求失败: ");
    Serial.println(http.errorToString(httpCode));
  }
  
  http.end();
  WiFi.disconnect(true);
  Serial.println("WiFi 断开完成");
}

// ========== 进入深度睡眠 ==========
void goToDeepSleep() {
  Serial.println("进入深度睡眠，功耗降至约 5μA...");
  digitalWrite(ledPin, LOW);
  esp_sleep_enable_timer_wakeup(60 * 1000000ULL);
  esp_deep_sleep_start();
}

// ========== 主程序 ==========
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(ledPin, OUTPUT);
  pinMode(10, OUTPUT);
  digitalWrite(10, HIGH);
  digitalWrite(ledPin, LOW);
  
  for (int i = 0; i < 3; i++) {
    digitalWrite(ledPin, HIGH);
    delay(100);
    digitalWrite(ledPin, LOW);
    delay(100);
  }
  
  Serial.println("=== 摔倒报警器启动（三段状态机）===");
  
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
  
  // 预热，让传感器稳定
  for (int i = 0; i < 10; i++) {
    readAccel();
    recordAccelSample();
    delay(50);
  }
  
  Serial.println("开始监控，等待摔倒事件...");
}

void loop() {
  readAccel();
  recordAccelSample();
  
  long accelSum = abs(ax) + abs(ay) + abs(az);
  
  // 调试输出：仅当数值异常或状态变化时打印（可自行调整）
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
      // 延时短暂避过活动期，然后进入静止等待
       if (millis() - impactTime > 500) {
        fallState = STILL_WAIT;
        stillStartTime = millis();
        // 记录静止基准（当前加速度）
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
    // 如果基准未设，马上设置
        baseAx = ax; baseAy = ay; baseAz = az;
        baseReady = true;
      }

      // 计算与基准的偏差绝对值
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
            
            for (int i = 0; i < 5; i++) {
              delay(500);
              digitalWrite(10, LOW);
              delay(500);
              digitalWrite(10, HIGH);
            }
            
            
            goToDeepSleep();
          }
          fallState = IDLE;   // 报警后复位状态
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
  
  delay(20);   // 50Hz 采样率
}