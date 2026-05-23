/*
 * 项目二：钥匙扣（广播端）
 * 功能：持续广播BLE信号，超低功耗
 * 硬件：ESP32-C3 + CR2032纽扣电池
 * 
 * 使用说明：
 * 1. 每个钥匙扣修改 KEYFOB_ID 为唯一编号（如 001, 002）
 * 2. 广播名称格式：KEYFOB_001
 * 3. 桩子通过前缀 "KEYFOB_" 识别
 */

#include <ArduinoBLE.h>

// ========== 钥匙扣配置 ==========
#define KEYFOB_ID "001"                    // 每个钥匙扣唯一编号，可以在后续对每个唯一名字做映射，对应具体是哪家的谁
const char* KEYFOB_NAME = "KEYFOB_" KEYFOB_ID;  // 广播名称：KEYFOB_001

// ========== LED 引脚（做演示使用，实际发行要去掉这些东西，不必然对功耗有影响）==========
const int ledPin = 8;

void setup() {
  // 可选：调试用串口（正式使用时可以注释掉以省电）
  Serial.begin(115200);
  
  pinMode(ledPin, OUTPUT);
  
  // 初始化 BLE
  if (!BLE.begin()) {
    Serial.println("初始化BLE失败");
    return;
  }
  Serial.println("初始化BLE成功");
  // 设置广播名称（桩子通过此前缀识别）
  BLE.setLocalName(KEYFOB_NAME);
  
  // 设置广播数据（不需要服务和特征值，纯广播即可）
  // 这样功耗最低
  BLE.advertise();
  
  // 启动提示：快闪2次，启动提示，万一没电了好知道
  for (int i = 0; i < 2; i++) {
    digitalWrite(ledPin, HIGH);
    delay(50);
    digitalWrite(ledPin, LOW);
    delay(50);
  }
  Serial.println("启动配置结束，准备循环");
}
 
void loop() {
  // 为了让 LED 偶尔闪烁指示工作状态（会增加功耗，后续可以去掉）
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 30000) {  // 每30秒闪一次
    digitalWrite(ledPin, HIGH);
    delay(10);
    digitalWrite(ledPin, LOW);
    lastBlink = millis();
  }
  
  delay(1000);
}