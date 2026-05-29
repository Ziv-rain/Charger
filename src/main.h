#pragma once

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <OneButton.h>
#include <BQ27220.h>

// ================= 引脚定义 =================
const int PIN_KEY1 = 34;
const int PIN_KEY2 = 35;

const int PIN_CHARGE_EN = 32;
const int PIN_CHARGE_CURRENT = 33;

const int PIN_DISCHARGE_EN = 25;
const int PIN_DISCHARGE_CURRENT = 26;

const int PIN_SD_MISO = 12;
const int PIN_SD_MOSI = 13;
const int PIN_SD_CLK = 14;
const int PIN_SD_CS = 15;
const int PIN_SD_DET = 18;

const int PIN_I2C_SDA = 21;
const int PIN_I2C_SCL = 22;

// ================= 全局对象 =================
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
extern OneButton button1;
extern OneButton button2;
extern BQ27220 fuelGauge;

// ================= 状态机 =================
enum SystemState {
    STATE_STOP,
    STATE_CHARGE_RUN,
    STATE_CHARGE_PAUSE,
    STATE_DISCHARGE_RUN,
    STATE_DISCHARGE_PAUSE
};

enum WorkMode {
    MODE_CHARGE,
    MODE_DISCHARGE
};

// ================= 事件类型 =================
#define EVENT_NONE               0
#define EVENT_MANUAL_START       1
#define EVENT_MANUAL_STOP        2
#define EVENT_MANUAL_GEAR        3
#define EVENT_MANUAL_MODE        4
#define EVENT_AUTO_CUTOFF_FULL   5
#define EVENT_AUTO_CUTOFF_EMPTY  6
#define EVENT_MANUAL_RESUME      7
#define EVENT_MANUAL_PAUSE       8

// ================= 全局状态变量 =================
extern SystemState currentState;
extern WorkMode currentMode;
extern int chargeGear;
extern int dischargeGear;

// ================= 外设状态 =================
extern bool bq27220_ok;
extern bool sd_card_ok;
extern bool bleDeviceConnected;
extern uint16_t bleChargeCutoff;
extern uint16_t bleDischargeCutoff;

// ================= 电池数据缓存 =================
extern int batterySOC;
extern int batteryVoltage;
extern int batteryCurrent;
extern float batteryTemp;
extern int batteryAvgCurrent;
extern int batteryRemainCap;
extern int batteryFullCap;
extern int batteryDesignCap;
extern int batteryCycleCount;
extern int batterySOH;
extern int batteryTTE;
extern int batteryTTF;
extern uint16_t batteryStatus;
extern uint16_t operationStatus;

// ================= 定时器 =================
extern unsigned long lastDisplayUpdate;
extern unsigned long lastSensorRead;
extern unsigned long lastSDLog;
extern unsigned long lastSDCheck;
extern unsigned long lastBQRecovery;

// ================= 故障计数 =================
extern int bqFailCount;
extern int sdFailCount;

// ================= 日志文件名 =================
extern char logFilename[32];

// ================= 事件标记 =================
extern uint8_t lastEvent;

// ================= 自定义SOC计算 =================
#define DESIGN_CAPACITY_MAH 1500
extern int customSOC;
extern float remainingCapacityMah;
extern unsigned long lastSocCalcTime;
extern unsigned long bq27220RunTime;        // BQ27220连续运行时间(ms)
extern bool bq27220NeedReset;               // BQ27220是否需要重置
extern unsigned long lastStateChangeTime;   // 上次状态变化时间
#define BQ27220_RESET_THRESHOLD 1800000     // 30分钟运行后需要重置(毫秒)

// ================= 库仑计数优化 =================
#define COULOMB_GAIN_INIT 0.95f              // 库仑计数增益系数初始值
#define CURRENT_FILTER_SIZE 3                // 电流滤波窗口大小
extern float currentFilterBuf[CURRENT_FILTER_SIZE];
extern int currentFilterIdx;
extern float coulombGain;                    // 动态增益系数

// ================= 自动校准 =================
extern unsigned long lastAutoCalibrateTime;
extern bool autoCalibrating;
extern bool fastCalibrateMode;               // 是否在快速校准模式
extern int lastBq27220Soc;                   // 上次BQ27220读数
extern int socDeviationCount;                // 偏差计数
#define AUTO_CALIBRATE_INTERVAL 900000       // 15分钟（正常间隔）
#define AUTO_CALIBRATE_FAST_INTERVAL 180000  // 3分钟（快速校准）
#define AUTO_CALIBRATE_STABLE_TIME 1500      // 电压稳定等待时间
#define CALIBRATE_CONVERGE_THRESHOLD 2       // 收敛阈值
#define COULOMB_GAIN_MIN 0.5f
#define COULOMB_GAIN_MAX 1.5f

// ================= 循环与能量统计 =================
extern int cycleNumber;
extern int phaseType;           // 0=休息, 1=充电, 2=放电
extern unsigned long phaseStartTime;
extern float cumulativeMahIn;
extern float cumulativeMahOut;
extern float cycleMahIn;
extern float cycleMahOut;
extern float cumulativeWh;
extern int lastVoltage;
extern unsigned long lastDvDtTime;
extern float dvDt;
extern float estimatedIR;

// ================= 函数声明 =================
void applyPowerControl();
void readBatteryData();
void updateOLED();
bool initSDCard();
void logToSDCard();
void clickKey1();
void longPressKey1();
void clickKey2();
void longPressKey2();

// BLE 通信
void initBLE();
bool bleIsConnected();
void blePollCommand();
void bleUpdateAndNotify();
