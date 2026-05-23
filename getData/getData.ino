/*
 * 项目二：记录收集器
 * 功能：
 *   1. 主动连接桩子下载数据（BLE客户端）
 *   2. 接受手机连接，响应 getdata 命令（BLE服务器）
 *   3. OLED 显示进度和数据
 */

#include <ArduinoBLE.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ========== OLED 配置 ==========
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ========== 桩子信息列表 ==========
struct FenceInfo {
  const char* name;
  const char* mac;
};

FenceInfo fences[] = {
  {"FENCE_001", "e8:3d:c1:90:5f:fa"},
};

const int fenceCount = sizeof(fences) / sizeof(fences[0]);

// ========== 桩子 BLE 服务 UUID ==========
const char* fenceServiceUuid = "19B10000-E8F2-537E-4F6C-D104768A1214";
const char* fenceDataCharUuid = "19B10002-E8F2-537E-4F6C-D104768A1216";

// ========== 收集设备的 BLE 服务（供手机连接）==========
const char* collectorServiceUuid = "19B20000-E8F2-537E-4F6C-D104768A1214";
const char* collectorCommandCharUuid = "19B20001-E8F2-537E-4F6C-D104768A1215";
const char* collectorDataCharUuid = "19B20002-E8F2-537E-4F6C-D104768A1216";

BLEService collectorService(collectorServiceUuid);
BLEStringCharacteristic commandCharacteristic(collectorCommandCharUuid, BLERead | BLEWrite, 32);
BLEStringCharacteristic dataCharacteristic(collectorDataCharUuid, BLERead | BLEWrite, 2048);

// ========== 存储收集到的所有数据 ==========
String allData = "";
String currentFenceData = "";

const int ledPin = 8;
bool deviceConnected = false;

// ========== OLED 显示函数 ==========
void showStartup() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 20);
  display.println("Record Collector");
  display.setCursor(30, 40);
  display.print("Fences: ");
  display.println(fenceCount);
  display.display();
  delay(2000);
}

void showScanning(int fenceIndex, int total) {
  display.clearDisplay();
  display.setCursor(0, 10);
  display.print("Scanning [");
  display.print(fenceIndex + 1);
  display.print("/");
  display.print(total);
  display.println("]");
  display.setCursor(0, 30);
  display.print("Target: ");
  display.println(fences[fenceIndex].name);
  display.setCursor(0, 50);
  display.println("Please wait...");
  display.display();
}

void showConnecting(int fenceIndex, int total) {
  display.clearDisplay();
  display.setCursor(0, 10);
  display.print("Connecting [");
  display.print(fenceIndex + 1);
  display.print("/");
  display.print(total);
  display.println("]");
  display.setCursor(0, 30);
  display.print("Target: ");
  display.println(fences[fenceIndex].name);
  display.setCursor(0, 50);
  display.println("Connecting...");
  display.display();
}

void showProgress(int current, int total) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.print("Downloading...");
  display.setCursor(0, 20);
  display.print("Progress: ");
  display.print(current);
  display.print("/");
  display.println(total);
  int barWidth = map(current, 0, total, 0, 120);
  display.drawRect(0, 40, 128, 10, SSD1306_WHITE);
  display.fillRect(0, 40, barWidth, 10, SSD1306_WHITE);
  display.display();
}

void showError(const char* msg) {
  display.clearDisplay();
  display.setCursor(0, 20);
  display.println("ERROR!");
  display.setCursor(0, 40);
  display.println(msg);
  display.display();
  delay(2000);
}

void showCollectorReady() {
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("Collector Ready");
  display.setCursor(0, 30);
  display.print("Data size: ");
  display.print(allData.length());
  display.println(" bytes");
  display.setCursor(0, 50);
  display.println("Connect phone ->");
  display.display();
}

void showPhoneConnected() {
  display.clearDisplay();
  display.setCursor(0, 25);
  display.println("Phone Connected");
  display.setCursor(0, 45);
  display.println("Send 'getdata'");
  display.display();
}

// ========== 设置收集设备的 BLE 服务 ==========
void setupCollectorService() {
  BLE.setLocalName("DataCollector");
  BLE.setAdvertisedService(collectorService);
  
  collectorService.addCharacteristic(commandCharacteristic);
  collectorService.addCharacteristic(dataCharacteristic);
  BLE.addService(collectorService);
  
  commandCharacteristic.writeValue("ready");
  dataCharacteristic.writeValue("");
  
  BLE.advertise();
  Serial.println("收集设备 BLE 服务已启动");
}

// ========== 从桩子下载数据 ==========
bool downloadFromFence(int fenceIndex, int total) {
  const char* fenceName = fences[fenceIndex].name;
  const char* fenceMac = fences[fenceIndex].mac;
  
  showScanning(fenceIndex, total);
  
  // 扫描指定 MAC 的设备
  BLE.scan();
  BLEDevice device;
  bool found = false;
  unsigned long startTime = millis();
  
  while (millis() - startTime < 5000) {
    device = BLE.available();
    if (device) {
      String addr = device.address();
      if (addr.equalsIgnoreCase(fenceMac)) {
        found = true;
        break;
      }
    }
  }
  BLE.stopScan();
  
  if (!found) {
    showError("Device not found");
    return false;
  }
  
  showConnecting(fenceIndex, total);
  
  if (!device.connect()) {
    showError("Connect failed");
    return false;
  }
  
  showProgress(30, 100);
  
  bool success = false;
  if (device.discoverAttributes()) {
    showProgress(60, 100);
    
    BLEService service = device.service(fenceServiceUuid);
    if (service) {
      BLECharacteristic dataChar = service.characteristic(fenceDataCharUuid);
      if (dataChar && dataChar.canRead()) {
        showProgress(90, 100);
        
        // 修正点：使用缓冲区读取
        uint8_t buffer[2048];
        int len = dataChar.readValue(buffer, sizeof(buffer));
        String jsonData = "";
        if (len > 0) {
          buffer[len] = '\0';
          jsonData = String((char*)buffer);
        }
        
        if (jsonData.length() > 0) {
          currentFenceData = jsonData;
          
          // 添加到总数据中
          if (allData.length() > 0 && allData != "[") {
            allData += ",";
          }
          allData += currentFenceData;
          
          showProgress(100, 100);
          success = true;
        } else {
          showError("Data empty");
        }
      } else {
        showError("Cannot read char");
      }
    } else {
      showError("Service not found");
    }
  } else {
    showError("Discover failed");
  }
  
  device.disconnect();
  return success;
}

// ========== 主动下载所有桩子数据 ==========
void downloadAllFences() {
  allData = "[";
  
  for (int i = 0; i < fenceCount; i++) {
    downloadFromFence(i, fenceCount);
    delay(1000);
  }
  
  allData += "]";
  
  Serial.println("所有桩子收集完成");
  Serial.print("总数据: ");
  Serial.println(allData);
  
  // 更新 BLE 特征值，供手机读取
  dataCharacteristic.writeValue(allData);
  
  showCollectorReady();
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
  
  // 初始化 OLED
  Wire.begin(4, 5);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED 初始化失败");
  }
  
  showStartup();
  
  // 初始化 BLE
  if (!BLE.begin()) {
    showError("BLE init fail");
    while (1);
  }
  
  // 设置收集设备的 BLE 服务器
  setupCollectorService();
  
  // 主动下载所有桩子数据
  downloadAllFences();
  
  Serial.println("=== 收集设备就绪，等待手机连接 ===");
}

void loop() {
  // 检查手机是否连接
  BLEDevice central = BLE.central();
  
  if (central) {
    if (!deviceConnected) {
      deviceConnected = true;
      Serial.println("手机已连接");
      showPhoneConnected();
    }
    
    // 在连接期间处理指令
    while (central.connected()) {
      // 检查是否有写入的特征值
      if (commandCharacteristic.written()) {
        String command = commandCharacteristic.value();
        Serial.print("收到指令: ");
        Serial.println(command);
        
        if (command == "getdata") {
          Serial.println("准备发送数据...");
          dataCharacteristic.writeValue(allData);
          Serial.println("数据发送完成");
        }
      }
      delay(100);
    }
    
    deviceConnected = false;
    Serial.println("手机已断开");
    showCollectorReady();
  }
  
  delay(100);
}