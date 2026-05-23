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

// 全局存储
var (
	alerts     []Alert
	alertsLock sync.RWMutex
	dataFile   = "data/alerts.json"
)

func main() {
	// 加载历史数据
	loadAlerts()

	// 启动定时保存（每30秒）
	go autoSave()

	// 静态文件服务
	http.Handle("/static/", http.StripPrefix("/static/", http.FileServer(http.Dir("static"))))

	// API 路由
	http.HandleFunc("/api/drop", handleDrop)     // ESP32 上报数据
	http.HandleFunc("/api/alerts", handleAlerts) // 获取报警列表
	http.HandleFunc("/api/latest", handleLatest) // 获取最新报警
	http.HandleFunc("/api/clear", handleClear)   // 清空报警

	// 首页
	http.HandleFunc("/", handleIndex)

	log.Println("服务器启动在 http://localhost:8080")
	log.Fatal(http.ListenAndServe(":8080", nil))
}

// 加载历史数据
func loadAlerts() {
	data, err := os.ReadFile(dataFile)
	if err != nil {
		log.Println("没有历史数据文件，将创建新文件")
		return
	}

	alertsLock.Lock()
	defer alertsLock.Unlock()

	err = json.Unmarshal(data, &alerts)
	if err != nil {
		log.Println("解析历史数据失败:", err)
	} else {
		log.Printf("已加载 %d 条历史报警记录", len(alerts))
	}
}

// 保存数据到文件
func saveAlerts() {
	alertsLock.RLock()
	data, err := json.MarshalIndent(alerts, "", "  ")
	alertsLock.RUnlock()

	if err != nil {
		log.Println("序列化数据失败:", err)
		return
	}

	err = os.WriteFile(dataFile, data, 0644)
	if err != nil {
		log.Println("保存数据失败:", err)
	}
}

// 自动保存（每30秒）
func autoSave() {
	ticker := time.NewTicker(30 * time.Second)
	for range ticker.C {
		saveAlerts()
	}
}

// 处理 ESP32 上报的掉落数据
func handleDrop(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "只支持 POST 请求", http.StatusMethodNotAllowed)
		return
	}

	// 解析 JSON
	var newAlert Alert
	err := json.NewDecoder(r.Body).Decode(&newAlert)
	if err != nil {
		http.Error(w, "JSON 解析失败: "+err.Error(), http.StatusBadRequest)
		return
	}

	// 添加服务器接收时间
	newAlert.Time = time.Now()
	newAlert.TimeStr = newAlert.Time.Format("2006-01-02 15:04:05")

	// 存储到全局变量
	alertsLock.Lock()
	// 新数据添加到开头（最新的在最前面）
	alerts = append([]Alert{newAlert}, alerts...)
	// 只保留最近 500 条
	if len(alerts) > 500 {
		alerts = alerts[:500]
	}
	alertsLock.Unlock()

	// 立即保存（重要数据）
	go saveAlerts()

	log.Printf("收到掉落报警: device=%s, accel=%v", newAlert.Device, newAlert.Accel)

	// 返回成功响应
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{
		"status":  "success",
		"message": "报警已接收",
	})
}

// 获取所有报警记录
func handleAlerts(w http.ResponseWriter, r *http.Request) {
	alertsLock.RLock()
	defer alertsLock.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(alerts)
}

// 获取最新一条报警
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

// 清空所有报警
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

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"status": "success"})
}

// 返回首页
func handleIndex(w http.ResponseWriter, r *http.Request) {
	http.ServeFile(w, r, "static/index.html")
}
