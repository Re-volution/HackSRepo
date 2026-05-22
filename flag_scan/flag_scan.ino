/*
 * 围栏桩子（ESP32原生BLE库版）
 * 功能：低占空比扫描钥匙扣 + EEPROM存储 + BLE数据下载
 * 硬件：ESP32-C3 
 */

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <EEPROM.h>
#include <esp_efuse.h>
#include <esp_mac.h>

// ========== 桩子身份配置 ==========
const char* FENCE_ID = "FENCE_001";

// ========== 钥匙扣配置（前缀匹配）==========
const char* KEYFOB_PREFIX = "KEYFOB_";
const unsigned long COOLDOWN_SECONDS = 25;


// ========== EEPROM 配置 ==========
#define EEPROM_SIZE 8192
struct Record {
  uint32_t timestamp;
  char     keyfobId[16];
  int8_t   rssi;
};
Record records[300];
int recordCount = 0;

// ========== BLE 服务 UUID ==========
#define SERVICE_UUID        "19B10000-E8F2-537E-4F6C-D104768A1214"
#define COUNT_CHAR_UUID     "19B10001-E8F2-537E-4F6C-D104768A1215"
#define DATA_CHAR_UUID      "19B10002-E8F2-537E-4F6C-D104768A1216"
#define FENCE_CHAR_UUID     "19B10003-E8F2-537E-4F6C-D104768A1217"

BLEServer* pServer = NULL;
BLECharacteristic* pCountCharacteristic = NULL;
BLECharacteristic* pDataCharacteristic = NULL;
BLECharacteristic* pFenceCharacteristic = NULL;

bool deviceConnected = false;

// 连接回调
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("收集设备已连接");
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("收集设备已断开");
    pServer->startAdvertising();  // 重新开始广播
  }
};

const int ledPin = 8;

// ========== 读取并打印 MAC 地址 ==========
void printMacAddress() {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  Serial.printf("桩子 MAC 地址: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ========== 保存记录 ==========
void saveRecord(const char* keyfobId, int8_t rssi) {
  if (recordCount >= 300) recordCount = 0;
  
  records[recordCount].timestamp = millis() / 1000;
  strncpy(records[recordCount].keyfobId, keyfobId, 15);
  records[recordCount].keyfobId[15] = '\0';
  records[recordCount].rssi = rssi;
  
  int addr = recordCount * sizeof(Record);
  EEPROM.put(addr, records[recordCount]);
  EEPROM.commit();
  
  recordCount++;
  
  // 更新 BLE 特征值
  if (pCountCharacteristic) {
    pCountCharacteristic->setValue(recordCount);
  }
  updateDataCharacteristic();
  
  Serial.print("记录 #");
  Serial.print(recordCount); 
  Serial.print(": ");
  Serial.print(keyfobId);
  Serial.print(" RSSI=");
  Serial.println(rssi);
}

// ========== 检查冷却时间 ==========
bool isInCooldown(const char* keyfobId) {
  unsigned long now = millis() / 1000;
  for (int i = recordCount - 1; i >= 0 && i >= recordCount - 20; i--) {
    if (strcmp(records[i].keyfobId, keyfobId) == 0) {
      if (now - records[i].timestamp < COOLDOWN_SECONDS) {
        return true;
      }
      break;
    }
  }
  return false;
}

// ========== 更新数据特征值 ==========
void updateDataCharacteristic() {
  if (!pDataCharacteristic) return;
  
  String jsonData = "{\"fence\":\"" + String(FENCE_ID) + 
                    "\",\"count\":" + String(recordCount) + 
                    ",\"records\":[";
  
  for (int i = 0; i < recordCount && i < 300; i++) {
    if (records[i].timestamp == 0 || records[i].keyfobId[0] == '\0') continue;
    if (i > 0) jsonData += ",";
    jsonData += "{\"t\":" + String(records[i].timestamp);
    jsonData += ",\"id\":\"" + String(records[i].keyfobId) + "\"";
    jsonData += ",\"rssi\":" + String(records[i].rssi) + "}";
    
    if (jsonData.length() > 1500) break;
  }
  jsonData += "]}";
  
  pDataCharacteristic->setValue(jsonData.c_str());
}

// ========== 设置 BLE 服务 ==========
void setupBLEService() {
  BLEDevice::init(FENCE_ID);
  
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  pCountCharacteristic = pService->createCharacteristic(
    COUNT_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  
  pDataCharacteristic = pService->createCharacteristic(
    DATA_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  
  pFenceCharacteristic = pService->createCharacteristic(
    FENCE_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  
  pCountCharacteristic->setValue(recordCount);
  pFenceCharacteristic->setValue(FENCE_ID);
  updateDataCharacteristic();
  
  pService->start();
  
  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->start();
  
  Serial.println("BLE 服务已就绪，广播中...");
}

// ========== 扫描钥匙扣 ==========
void scanKeyfobs() {

  BLEScan* pBLEScan = BLEDevice::getScan();

  pBLEScan->setActiveScan(true);

  pBLEScan->start(1, false);

  BLEScanResults foundDevices = pBLEScan->getResults();
  for (int i = 0; i < foundDevices.getCount(); i++) {
    BLEAdvertisedDevice device = foundDevices.getDevice(i);
    String name = device.getName().c_str();

    if (name.startsWith(KEYFOB_PREFIX)) {
      digitalWrite(ledPin, HIGH);
      delay(10);
      digitalWrite(ledPin, LOW);
      
      if (!isInCooldown(name.c_str())) {
        saveRecord(name.c_str(), device.getRSSI());
      }
    }
  }
  
  pBLEScan->clearResults();
}

// ========== 主程序 ==========
void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  
  for (int i = 0; i < 3; i++) {
    digitalWrite(ledPin, HIGH);
    delay(100);
    digitalWrite(ledPin, LOW);
    delay(100);
  }
  
  Serial.println("=== 围栏桩子启动（原生BLE库版）===");
  printMacAddress();
  Serial.print("桩子 ID: ");
  Serial.println(FENCE_ID);
  
  EEPROM.begin(EEPROM_SIZE);
  
  for (int i = 0; i < 300; i++) {
    int addr = i * sizeof(Record);
    EEPROM.get(addr, records[i]);
    if (records[i].timestamp > 0 && 
        records[i].timestamp < 1000000000 &&  // 合理的时间范围
        records[i].keyfobId[0] != '\0') {
      recordCount++;
    } else {
      // 遇到空记录，停止读取（假设后续都是空的）
      break;
    }
  }
  Serial.print("已加载 ");
  Serial.print(recordCount);
  Serial.println(" 条历史记录");
  
  setupBLEService();
  Serial.println("进入主循环（低占空比扫描 + BLE服务）");
}

void loop() {

  scanKeyfobs();
  delay(500);
}