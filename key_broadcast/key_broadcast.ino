/*
 * 项目二：钥匙扣（超低功耗广播版）
 * 功能：每隔一段时间广播一次，其余时间深度睡眠
 * 硬件：ESP32-C3 + 纽扣电池
 */

#include <ArduinoBLE.h>
#include <esp_sleep.h>

// ========== 钥匙扣配置 ==========
#define KEYFOB_ID "001"
const char* KEYFOB_NAME = "KEYFOB_" KEYFOB_ID;

// 功耗优化参数
const unsigned long SLEEP_INTERVAL_SEC = 8;   // 睡眠间隔（秒）
const unsigned long BROADCAST_DURATION_MS = 100; // 广播持续时间（毫秒）

const int ledPin = 8;
unsigned long broadcastCount = 0;

// ========== 执行单次广播 ==========
void doOneBroadcast() {
  // 初始化 BLE
  if (!BLE.begin()) {
    Serial.println("BLE 初始化失败");
    return;
  }
  
  // 设置设备名称
  BLE.setLocalName(KEYFOB_NAME);
  
  // 开始广播
  BLE.advertise();
  
  Serial.print("广播名称: ");
  Serial.println(KEYFOB_NAME);
  
  // 广播一段时间
  delay(BROADCAST_DURATION_MS);
  
  // 注意：不要调用 BLE.end()，直接进入睡眠
  // 外设会在深度睡眠时自动断电
}

// ========== 进入深度睡眠 ==========
void goToDeepSleep() {
  // 短暂延迟确保所有操作完成
  delay(10);
  
  // 设置定时器唤醒
  esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL_SEC * 1000000ULL);
  
  // 进入深度睡眠
  esp_deep_sleep_start();
}

// ========== 主程序 ==========
void setup() {
  Serial.begin(115200);
  delay(100);
  
  // LED 指示
  pinMode(ledPin, OUTPUT);
  for (int i = 0; i < 2; i++) {
    digitalWrite(ledPin, HIGH);
    delay(50);
    digitalWrite(ledPin, LOW);
    delay(50);
  }
  
  Serial.println("=== 钥匙扣启动 ===");
  Serial.print("广播间隔: ");
  Serial.print(SLEEP_INTERVAL_SEC);
  Serial.println(" 秒");
  
  // 执行广播
  doOneBroadcast();
  
  broadcastCount++;
  Serial.print("广播 #");
  Serial.print(broadcastCount);
  Serial.print(" 完成，睡眠 ");
  Serial.print(SLEEP_INTERVAL_SEC);
  Serial.println(" 秒");
  
  // 进入深度睡眠
  goToDeepSleep();
}

void loop() {
  // 不会执行到这里
}