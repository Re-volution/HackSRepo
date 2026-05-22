/*
 * 项目二：记录收集器（修正版）
 * 功能：通过 MAC 地址连接围栏桩子，下载记录
 */

#include <ArduinoBLE.h>
 
// ========== 桩子信息列表 ==========
struct FenceInfo {
  const char* name;
  const char* mac;
};

// 请将每个桩子的 MAC 地址填入这里（从桩子串口输出获取）
FenceInfo fences[] = {
  {"FENCE_001", "e8:3d:c1:90:5f:fa"},   // 替换为实际 MAC
  // 新增桩子只需在这里添加
};

const int fenceCount = sizeof(fences) / sizeof(fences[0]);

// ========== BLE 服务 UUID（需与桩子一致）==========
const char* serviceUuid = "19B10000-E8F2-537E-4F6C-D104768A1214";
const char* dataCharUuid = "19B10002-E8F2-537E-4F6C-D104768A1216";

const int ledPin = 8;

// ========== 从指定桩子下载数据 ==========
bool downloadFromFence(const char* fenceName, const char* fenceMac) {
  Serial.print("连接桩子: ");
  Serial.print(fenceName);
  Serial.print(" (");
  Serial.print(fenceMac);
  Serial.println(")");
  
  // 1. 扫描找到指定 MAC 的设备
  BLE.scan();
  BLEDevice device;
  bool found = false;
  unsigned long startTime = millis();
  
  Serial.println("扫描中...");
  while (millis() - startTime < 5000) {  // 扫描5秒
    device = BLE.available();
    if (device) {
      String addr = device.address();
      if (addr.equalsIgnoreCase(fenceMac)) {
        found = true;
        Serial.print("找到设备: ");
        Serial.println(addr);
        break;
      }
    }
  }
  BLE.stopScan();
  
  if (!found) {
    Serial.println("未找到桩子设备，请检查 MAC 地址");
    return false;
  }
  
  // 2. 连接设备
  if (!device.connect()) {
    Serial.println("连接失败");
    return false;
  }
  
  Serial.println("连接成功");
  
  // 3. 发现属性和读取数据
  bool success = false;
  if (device.discoverAttributes()) {
    BLEService service = device.service(serviceUuid);
    if (service) {
      BLECharacteristic dataChar = service.characteristic(dataCharUuid);
      if (dataChar && dataChar.canRead()) {
        // 读取字符串数据（使用缓冲区）
        uint8_t buffer[2048];
        int len = dataChar.readValue(buffer, sizeof(buffer));
        String jsonData = "";
        if (len > 0) {
          jsonData = String((char*)buffer);
        }
        Serial.println("=== " + String(fenceName) + " 的记录 ===");
        Serial.println(jsonData);
        Serial.println("=========================");
        success = true;
        
        // 闪烁 LED 表示成功
        for (int i = 0; i < 3; i++) {
          digitalWrite(ledPin, HIGH);
          delay(100);
          digitalWrite(ledPin, LOW);
          delay(100);
        }
      } else {
        Serial.println("无法读取数据特征值");
      }
    } else {
      Serial.println("未找到下载服务");
    }
  } else {
    Serial.println("属性发现失败");
  }
  
  device.disconnect();
  return success;
}

// ========== 主程序 ==========
void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  
  // 启动提示
  for (int i = 0; i < 3; i++) {
    digitalWrite(ledPin, HIGH);
    delay(100);
    digitalWrite(ledPin, LOW);
    delay(100);
  }
  
  Serial.println("=== 记录收集器启动 ===");
  Serial.print("共 ");
  Serial.print(fenceCount);
  Serial.println(" 个桩子待收集");
  
  if (!BLE.begin()) {
    Serial.println("BLE 初始化失败");
    while (1);
  }
  
  Serial.println("BLE 初始化成功");
}

void loop() {
  Serial.println("\n开始收集...");
  
  for (int i = 0; i < fenceCount; i++) {
    downloadFromFence(fences[i].name, fences[i].mac);
    delay(1000);  // 桩子之间的间隔
  }
  
  Serial.println("\n所有桩子收集完成");
  Serial.println("等待 5 分钟后再次扫描...\n");
  
  // 等待 5 分钟（可调整）
  for (int i = 0; i < 300; i++) {
    delay(1000);
    if (i % 60 == 0) {
      digitalWrite(ledPin, HIGH);
      delay(50);
      digitalWrite(ledPin, LOW);
    }
  }
}