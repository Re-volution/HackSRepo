package main

import (
	"encoding/json"
	"log"
	"net/http"
	"os"
	"sync"
	"time"
)

// Alert 掉落报警数据结构
type Alert struct {
	Device    string    `json:"device"`
	Event     string    `json:"event"`
	Timestamp int64     `json:"timestamp"`
	Accel     [3]int16  `json:"accel"`
	Time      time.Time `json:"time"`
	TimeStr   string    `json:"time_str"`
}

// Heartbeat 心跳数据结构
type Heartbeat struct {
	Device    string    `json:"device"`
	Type      string    `json:"type"`
	Timestamp int64     `json:"timestamp"`
	Time      time.Time `json:"time"`
	TimeStr   string    `json:"time_str"`
}

// DeviceStatus 设备状态
type DeviceStatus struct {
	DeviceUUID    string    `json:"device_uuid"`
	LastHeartbeat time.Time `json:"last_heartbeat"`
	LastAlert     time.Time `json:"last_alert"`
	Status        string    `json:"status"`
}

// 全局存储
var (
	alerts       []Alert
	heartbeats   []Heartbeat
	deviceStatus map[string]*DeviceStatus
	alertsLock   sync.RWMutex
	dataFile     = "data/alerts.json"
	devicesFile  = "data/devices.json"
)

func init() {
	deviceStatus = make(map[string]*DeviceStatus)
}

func main() {
	loadAlerts()
	loadDevices()

	// 启动告警检查协程
	go alertChecker()

	// 静态文件服务
	http.Handle("/static/", http.StripPrefix("/static/", http.FileServer(http.Dir("static"))))

	// API 路由
	http.HandleFunc("/api/drop", handleDrop)
	http.HandleFunc("/api/heartbeat", handleHeartbeat)
	http.HandleFunc("/api/alerts", handleAlerts)
	http.HandleFunc("/api/devices", handleDevices)
	http.HandleFunc("/api/device/add", handleDeviceAdd)
	http.HandleFunc("/api/device/remove", handleDeviceRemove)
	http.HandleFunc("/api/latest", handleLatest)
	http.HandleFunc("/api/clear", handleClear)

	http.HandleFunc("/", handleIndex)

	log.Println("服务器启动在 http://localhost:8080")
	log.Fatal(http.ListenAndServe(":8080", nil))
}

// ========== 文件操作函数 ==========

// 读取文件内容
func readFile(filePath string) ([]byte, error) {
	return os.ReadFile(filePath)
}

// 写入文件内容
func writeFile(filePath string, data []byte) error {
	return os.WriteFile(filePath, data, 0644)
}

// ========== 数据加载与保存 ==========

func loadAlerts() {
	data, err := readFile(dataFile)
	if err != nil {
		if os.IsNotExist(err) {
			log.Println("没有历史数据文件，将创建新文件")
		} else {
			log.Println("读取历史数据失败:", err)
		}
		return
	}

	alertsLock.Lock()
	defer alertsLock.Unlock()
	if err := json.Unmarshal(data, &alerts); err != nil {
		log.Println("解析历史数据失败:", err)
	} else {
		log.Printf("已加载 %d 条历史报警记录", len(alerts))
	}
}

func saveAlerts() {
	alertsLock.RLock()
	data, err := json.MarshalIndent(alerts, "", "  ")
	alertsLock.RUnlock()

	if err != nil {
		log.Println("序列化报警数据失败:", err)
		return
	}

	if err := writeFile(dataFile, data); err != nil {
		log.Println("保存报警数据失败:", err)
	}
}

func loadDevices() {
	data, err := readFile(devicesFile)
	if err != nil {
		if os.IsNotExist(err) {
			log.Println("没有设备列表文件，将创建新文件")
		} else {
			log.Println("读取设备列表失败:", err)
		}
		return
	}

	var devices []DeviceStatus
	if err := json.Unmarshal(data, &devices); err != nil {
		log.Println("解析设备列表失败:", err)
		return
	}

	alertsLock.Lock()
	defer alertsLock.Unlock()
	deviceStatus = make(map[string]*DeviceStatus)
	for i := range devices {
		deviceStatus[devices[i].DeviceUUID] = &devices[i]
	}
	log.Printf("已加载 %d 个设备", len(deviceStatus))
}

func saveDevices() {
	var devices []DeviceStatus
	for _, status := range deviceStatus {
		devices = append(devices, *status)
	}
	data, err := json.MarshalIndent(devices, "", "  ")
	if err != nil {
		log.Println("序列化设备数据失败:", err)
		return
	}
	if err := writeFile(devicesFile, data); err != nil {
		log.Println("保存设备数据失败:", err)
	}
}

// ========== 告警检查 ==========

func alertChecker() {
	ticker := time.NewTicker(1 * time.Minute)
	for range ticker.C {
		alertsLock.Lock()
		now := time.Now()
		for uuid, status := range deviceStatus {
			if now.Sub(status.LastHeartbeat) > 24*time.Hour {
				if status.Status != "offline" {
					status.Status = "offline"
					log.Printf("⚠️ 设备 %s 离线超过24小时！", uuid)
					saveDevices()
				}
			} else {
				if status.Status != "online" {
					status.Status = "online"
					log.Printf("设备 %s 已上线", uuid)
					saveDevices()
				}
			}
		}
		alertsLock.Unlock()
	}
}

// ========== API 处理函数 ==========

func handleDrop(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "只支持 POST 请求", http.StatusMethodNotAllowed)
		return
	}

	var alert Alert
	if err := json.NewDecoder(r.Body).Decode(&alert); err != nil {
		http.Error(w, "JSON 解析失败", http.StatusBadRequest)
		return
	}

	alert.Time = time.Now()
	alert.TimeStr = alert.Time.Format("2006-01-02 15:04:05")

	alertsLock.Lock()
	alerts = append([]Alert{alert}, alerts...)
	if len(alerts) > 500 {
		alerts = alerts[:500]
	}

	// 更新设备最后报警时间
	if _, exists := deviceStatus[alert.Device]; exists {
		deviceStatus[alert.Device].LastAlert = alert.Time
		deviceStatus[alert.Device].Status = "online"
		saveDevices()
	}
	alertsLock.Unlock()

	go saveAlerts()

	log.Printf("收到掉落报警: device=%s", alert.Device)

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"status": "success"})
}

func handleHeartbeat(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "只支持 POST 请求", http.StatusMethodNotAllowed)
		return
	}

	var hb Heartbeat
	if err := json.NewDecoder(r.Body).Decode(&hb); err != nil {
		http.Error(w, "JSON 解析失败", http.StatusBadRequest)
		return
	}

	hb.Time = time.Now()
	hb.TimeStr = hb.Time.Format("2006-01-02 15:04:05")

	alertsLock.Lock()
	heartbeats = append(heartbeats, hb)
	if len(heartbeats) > 100 {
		heartbeats = heartbeats[1:]
	}

	// 更新设备心跳时间
	if _, exists := deviceStatus[hb.Device]; !exists {
		deviceStatus[hb.Device] = &DeviceStatus{
			DeviceUUID:    hb.Device,
			LastHeartbeat: hb.Time,
			Status:        "online",
		}
		saveDevices()
	} else {
		deviceStatus[hb.Device].LastHeartbeat = hb.Time
		deviceStatus[hb.Device].Status = "online"
		saveDevices()
	}
	alertsLock.Unlock()

	log.Printf("收到心跳: %s at %s", hb.Device, hb.TimeStr)

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

func handleAlerts(w http.ResponseWriter, r *http.Request) {
	alertsLock.RLock()
	defer alertsLock.RUnlock()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(alerts)
}

func handleLatest(w http.ResponseWriter, r *http.Request) {
	alertsLock.RLock()
	defer alertsLock.RUnlock()
	if len(alerts) == 0 {
		json.NewEncoder(w).Encode(map[string]string{"status": "no_data"})
		return
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(alerts[0])
}

func handleClear(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "只支持 POST 请求", http.StatusMethodNotAllowed)
		return
	}
	alertsLock.Lock()
	alerts = []Alert{}
	alertsLock.Unlock()
	saveAlerts()
	log.Println("报警记录已清空")
	json.NewEncoder(w).Encode(map[string]string{"status": "success"})
}

func handleDevices(w http.ResponseWriter, r *http.Request) {
	alertsLock.RLock()
	defer alertsLock.RUnlock()
	var devices []DeviceStatus
	for _, status := range deviceStatus {
		devices = append(devices, *status)
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(devices)
}

func handleDeviceAdd(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "只支持 POST 请求", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		UUID string `json:"uuid"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "JSON 解析失败", http.StatusBadRequest)
		return
	}
	if req.UUID == "" {
		http.Error(w, "UUID 不能为空", http.StatusBadRequest)
		return
	}

	alertsLock.Lock()
	defer alertsLock.Unlock()

	// 检查是否已存在
	if _, exists := deviceStatus[req.UUID]; exists {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusConflict) // 409 Conflict
		json.NewEncoder(w).Encode(map[string]string{"status": "error", "message": "设备已存在"})
		return
	}

	deviceStatus[req.UUID] = &DeviceStatus{
		DeviceUUID:    req.UUID,
		LastHeartbeat: time.Now(),
		Status:        "unknown",
	}
	saveDevices()
	log.Printf("添加设备: %s", req.UUID)

	json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

func handleDeviceRemove(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "只支持 POST 请求", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		UUID string `json:"uuid"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "JSON 解析失败", http.StatusBadRequest)
		return
	}

	alertsLock.Lock()
	delete(deviceStatus, req.UUID)
	saveDevices()
	log.Printf("删除设备: %s", req.UUID)
	alertsLock.Unlock()

	json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

func handleIndex(w http.ResponseWriter, r *http.Request) {
	http.ServeFile(w, r, "static/index.html")
}
