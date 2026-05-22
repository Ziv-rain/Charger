#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <OneButton.h>
#include <BQ27220.h>
#include <SD.h>
#include <SPI.h>

// ================= 引脚定义 =================
const int PIN_KEY1 = 34;             // 按键1 (IO34, 低电平有效)
const int PIN_KEY2 = 35;             // 按键2 (IO35, 低电平有效)

// 充电控制
const int PIN_CHARGE_EN = 32;        // TP4056使能 (HIGH=使能充电)
const int PIN_CHARGE_CURRENT = 33;   // 充电电流 (HIGH=1000mA, LOW=500mA)

// 放电控制
const int PIN_DISCHARGE_EN = 25;     // CN5711使能 (HIGH=使能放电)
const int PIN_DISCHARGE_CURRENT = 26;// 放电电流 (HIGH=500mA, LOW=100mA)

// SD卡 SPI
const int PIN_SD_MISO = 12;
const int PIN_SD_MOSI = 13;
const int PIN_SD_CLK = 14;
const int PIN_SD_CS = 15;
const int PIN_SD_DET = 18;           // SD卡检测 (LOW=已插入)

// ================= 初始化对象 =================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
OneButton button1(PIN_KEY1, true, false);
OneButton button2(PIN_KEY2, true, false);
BQ27220 fuelGauge;

// ================= 状态机枚举定义 =================
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

// 事件类型 (供 AI 训练时过滤充放电切换瞬间的不稳定数据)
#define EVENT_NONE               0   // 无事件
#define EVENT_MANUAL_START       1   // 按键启动充放电
#define EVENT_MANUAL_STOP        2   // 按键停止 / 紧急停止
#define EVENT_MANUAL_GEAR        3   // 按键切换档位
#define EVENT_MANUAL_MODE        4   // 按键切换充电/放电模式
#define EVENT_AUTO_CUTOFF_FULL   5   // 自动截止: 充满 (>=4.2V)
#define EVENT_AUTO_CUTOFF_EMPTY  6   // 自动截止: 电量不足 (<=3.0V)

// ================= 全局变量 =================
SystemState currentState = STATE_STOP;
WorkMode currentMode = MODE_CHARGE;
int chargeGear = 1;
int dischargeGear = 1;

// 外设状态
bool bq27220_ok = false;
bool sd_card_ok = false;
bool bt_connected = false;     // 蓝牙连接状态 (预留)

// 电池数据缓存
int batterySOC = -1;              // %
int batteryVoltage = -1;          // mV
int batteryCurrent = 0;           // mA
float batteryTemp = NAN;          // °C
int batteryAvgCurrent = 0;        // mA
int batteryRemainCap = -1;        // mAh
int batteryFullCap = -1;          // mAh
int batteryDesignCap = -1;        // mAh
int batteryCycleCount = -1;       // 次
int batterySOH = -1;              // %
int batteryTTE = -1;              // min
int batteryTTF = -1;              // min
uint16_t batteryStatus = 0;       // 状态标志位(16bit)
uint16_t operationStatus = 0;     // 操作状态标志位(16bit)

// 定时器
unsigned long lastDisplayUpdate = 0;
unsigned long lastSensorRead = 0;
unsigned long lastSDLog = 0;
unsigned long lastSDCheck = 0;
unsigned long lastBQRecovery = 0;

// BQ27220 故障计数
int bqFailCount = 0;

// SD 卡故障计数 (连续失败 N 次才标记故障)
int sdFailCount = 0;

// 日志文件名 (每次上电自动生成新文件, 避免多次采集混在一起)
char logFilename[32] = "/charger.csv";

// 最近一次事件 (写入 CSV 后自动清零, 供 AI 过滤过渡数据)
uint8_t lastEvent = EVENT_NONE;

// ================= 充放电硬件控制 (互锁) =================
void applyPowerControl() {
    bool chargeOn = (currentState == STATE_CHARGE_RUN);
    bool dischargeOn = (currentState == STATE_DISCHARGE_RUN);

    // 互锁：充电和放电严禁同时使能
    if (chargeOn && dischargeOn) {
        digitalWrite(PIN_CHARGE_EN, LOW);
        digitalWrite(PIN_DISCHARGE_EN, LOW);
        return;
    }

    // 统一时序：先关 → 设电流 → 延迟稳定 → 再开
    // 避免 TP4056/CN5711 在使能状态下切换电流设定电阻，导致内部环路异常
    if (chargeOn) {
        digitalWrite(PIN_CHARGE_EN, LOW);          // 1. 先关充电
        digitalWrite(PIN_DISCHARGE_EN, LOW);        // 2. 确保放电关闭
        digitalWrite(PIN_CHARGE_CURRENT, (chargeGear == 2) ? HIGH : LOW); // 3. 设定电流电阻
        delayMicroseconds(500);                     // 4. 等待 MOSFET 切换稳定
        digitalWrite(PIN_CHARGE_EN, HIGH);           // 5. 重新使能充电
    } else if (dischargeOn) {
        digitalWrite(PIN_DISCHARGE_EN, LOW);         // 1. 先关放电
        digitalWrite(PIN_CHARGE_EN, LOW);            // 2. 确保充电关闭
        digitalWrite(PIN_DISCHARGE_CURRENT, (dischargeGear == 2) ? HIGH : LOW); // 3. 设定电流电阻
        delayMicroseconds(500);                      // 4. 等待 MOSFET 切换稳定
        digitalWrite(PIN_DISCHARGE_EN, HIGH);         // 5. 重新使能放电
    } else {
        digitalWrite(PIN_CHARGE_EN, LOW);
        digitalWrite(PIN_DISCHARGE_EN, LOW);
    }
}

// ================= SD 卡 =================
bool initSDCard() {
    SPI.begin(PIN_SD_CLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    SPI.setFrequency(25000000);  // SD卡SPI模式最高稳定频率25MHz
    delay(10);                   // 等待SPI总线稳定

    if (!SD.begin(PIN_SD_CS)) {
        Serial.println("SD卡初始化失败！");
        return false;
    }

    // 自动查找下一个可用文件名 (charger_001.csv, charger_002.csv, ...)
    int n = 1;
    while (n <= 999) {
        snprintf(logFilename, sizeof(logFilename), "/charger_%03d.csv", n);
        if (!SD.exists(logFilename)) break;
        n++;
    }
    if (n > 999) {
        // 兜底：超过999个文件时使用时间戳命名
        snprintf(logFilename, sizeof(logFilename), "/charger_%lu.csv", millis());
    }

    // 只有新建文件才写表头 (SD.exists 已保证文件不存在)
    File logFile = SD.open(logFilename, FILE_WRITE);
    if (logFile) {
        logFile.println("timestamp_ms,state_id,gear_id,"
                        "SOC_pct,voltage_mV,current_mA,avg_current_mA,"
                        "temperature_C,remain_cap_mAh,full_cap_mAh,design_cap_mAh,"
                        "cycle_count_n,SOH_pct,tte_min,ttf_min,"
                        "batt_status_hex,op_status_hex,event_id");
        logFile.close();
        Serial.printf("SD卡初始化成功 -> %s\n", logFilename);
        sdFailCount = 0;
    } else {
        Serial.println("SD卡创建文件失败！");
        return false;
    }
    return true;
}

void logToSDCard() {
    if (!sd_card_ok) return;
    File logFile = SD.open(logFilename, FILE_APPEND);
    if (!logFile) {
        // 单次失败不立即标记故障, 累计连续失败再降级
        sdFailCount++;
        if (sdFailCount > 3) {
            sd_card_ok = false;
            sdFailCount = 0;
            Serial.println("SD卡连续写入失败，暂停记录！");
        }
        return;
    }
    sdFailCount = 0;  // 写入成功, 清零连续失败计数

    logFile.print(millis());
    logFile.print(",");
    logFile.print(currentState);
    logFile.print(",");
    logFile.print((currentMode == MODE_CHARGE) ? chargeGear : dischargeGear);
    logFile.print(",");
    if (bq27220_ok) {
        logFile.print(batterySOC);
        logFile.print(",");
        logFile.print(batteryVoltage);
        logFile.print(",");
        logFile.print(batteryCurrent);
        logFile.print(",");
        logFile.print(batteryAvgCurrent);
        logFile.print(",");
        if (isnan(batteryTemp)) logFile.print("NAN"); else logFile.print(batteryTemp, 1);
        logFile.print(",");
        logFile.print(batteryRemainCap);
        logFile.print(",");
        logFile.print(batteryFullCap);
        logFile.print(",");
        logFile.print(batteryDesignCap);
        logFile.print(",");
        logFile.print(batteryCycleCount);
        logFile.print(",");
        logFile.print(batterySOH);
        logFile.print(",");
        logFile.print(batteryTTE);
        logFile.print(",");
        logFile.print(batteryTTF);
        logFile.print(",");
        { char h[7]; snprintf(h, 7, "0x%04X", batteryStatus);     logFile.print(h); }
        logFile.print(",");
        { char h[7]; snprintf(h, 7, "0x%04X", operationStatus);   logFile.print(h); }
        logFile.print(",");
        logFile.println(lastEvent);
    } else {
        // BQ27220 故障时用 -1/NAN/0x0000 占位
        logFile.println("-1,-1,-1,-1,NAN,-1,-1,-1,-1,-1,-1,-1,0x0000,0x0000," + String(lastEvent));
    }
    lastEvent = EVENT_NONE;  // 写入后清零
    logFile.close();
}

// ================= BQ27220 传感器读取 (14种数据) =================
void readBatteryData() {
    if (!bq27220_ok) return;

    // 1-3: 基本电气参数
    int soc = fuelGauge.readStateOfChargePercent();
    int mv  = fuelGauge.readVoltageMillivolts();
    int ma  = fuelGauge.readCurrentMilliamps();

    if (soc < 0 && mv < 0) {
        bqFailCount++;
        if (bqFailCount > 3) {
            bq27220_ok = false;
            Serial.println("BQ27220 通信故障！");
        }
        return;
    }
    bqFailCount = 0;
    batterySOC = soc;
    batteryVoltage = mv;
    batteryCurrent = ma;

    // 4: 电池温度
    float tempC = fuelGauge.readTemperatureCelsius();
    batteryTemp = isnan(tempC) ? NAN : tempC;

    // 5: 平均电流 (平滑值，更适合AI训练)
    int avgMa = fuelGauge.readAverageCurrentMilliamps();
    batteryAvgCurrent = (avgMa == INT16_MIN) ? 0 : avgMa;

    // 6-8: 容量相关
    batteryRemainCap = fuelGauge.readRemainingCapacitymAh();
    batteryFullCap   = fuelGauge.readFullChargeCapacitymAh();
    batteryDesignCap = fuelGauge.readDesignCapacitymAh();

    // 9-10: 老化相关
    batteryCycleCount = fuelGauge.readCycleCount();
    batterySOH = fuelGauge.readStateOfHealthPercent();

    // 11-12: 时间预测
    batteryTTE = fuelGauge.readTimeToEmptyMinutes();
    batteryTTF = fuelGauge.readTimeToFullMinutes();

    // 13-14: 状态标志位
    fuelGauge.readBatteryStatus(batteryStatus);
    fuelGauge.readOperationStatus(operationStatus);
}

// ================= OLED 显示刷新 =================
void updateOLED() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);

    // 第1行：图标栏 + 剩余容量 (y=10, 黄色区域 0-15)
    u8g2.setCursor(0, 10);
    u8g2.print(sd_card_ok ? "SD" : "--");
    u8g2.setCursor(30, 10);
    u8g2.print(bt_connected ? "BT" : "--");
    u8g2.setCursor(78, 10);
    if (bq27220_ok && batteryRemainCap >= 0) {
        u8g2.print(String(batteryRemainCap) + "mAh");
    } else {
        u8g2.print("----mAh");
    }

    // 第2行：模式 | 档位 | 状态 (y=28)
    u8g2.setCursor(0, 28);
    String line2 = "";
    line2 += (currentMode == MODE_CHARGE) ? "充电模式 | " : "放电模式 | ";
    line2 += String((currentMode == MODE_CHARGE) ? chargeGear : dischargeGear) + "档 | ";
    switch (currentState) {
        case STATE_STOP:             line2 += "停止"; break;
        case STATE_CHARGE_RUN:
        case STATE_DISCHARGE_RUN:    line2 += "运行"; break;
        case STATE_CHARGE_PAUSE:
        case STATE_DISCHARGE_PAUSE:  line2 += "暂停"; break;
    }
    u8g2.print(line2.c_str());

    // 第3行：SOC + 温度 (y=42)
    u8g2.setCursor(0, 42);
    if (bq27220_ok && batterySOC >= 0) {
        u8g2.print("SOC:" + String(batterySOC) + "%");
    } else if (!bq27220_ok) {
        u8g2.print("SOC:--% [ERR]");
    } else {
        u8g2.print("SOC:--%");
    }
    u8g2.setCursor(72, 42);
    if (bq27220_ok && !isnan(batteryTemp)) {
        u8g2.print("T:" + String(batteryTemp, 1) + "C");
    }

    // 第4行：电压 + 电流 (y=56)
    u8g2.setCursor(0, 56);
    if (bq27220_ok && batteryVoltage >= 0) {
        u8g2.print("V:" + String(batteryVoltage / 1000.0, 2) + "V");
        u8g2.setCursor(72, 56);
        u8g2.print("I:" + String(batteryCurrent) + "mA");
    } else if (!bq27220_ok) {
        u8g2.print("V:--V  I:--mA");
    }

    u8g2.sendBuffer();
}

// ================= 按键事件回调 =================

void clickKey1() {
    Serial.println("Key1 短按触发");
    bool fromStop = (currentState == STATE_STOP);
    switch (currentState) {
        case STATE_STOP:
            currentState = (currentMode == MODE_CHARGE) ? STATE_CHARGE_RUN : STATE_DISCHARGE_RUN;
            break;
        case STATE_CHARGE_RUN:
            currentState = STATE_CHARGE_PAUSE;
            break;
        case STATE_CHARGE_PAUSE:
            currentState = STATE_CHARGE_RUN;
            break;
        case STATE_DISCHARGE_RUN:
            currentState = STATE_DISCHARGE_PAUSE;
            break;
        case STATE_DISCHARGE_PAUSE:
            currentState = STATE_DISCHARGE_RUN;
            break;
    }
    // 运行态 ↔ 暂停态视为启停事件, 方便 AI 筛选稳定段
    lastEvent = fromStop ? EVENT_MANUAL_START : EVENT_MANUAL_STOP;
    applyPowerControl();
    if (sd_card_ok) logToSDCard();
    updateOLED();
}

void longPressKey1() {
    Serial.println("Key1 长按释放触发 -> 紧急停止");
    if (currentState != STATE_STOP) {
        currentState = STATE_STOP;
        lastEvent = EVENT_MANUAL_STOP;
        applyPowerControl();
        if (sd_card_ok) logToSDCard();
        updateOLED();
    }
}

void clickKey2() {
    Serial.println("Key2 短按触发 -> 切换档位");
    if (currentMode == MODE_CHARGE) {
        chargeGear = (chargeGear == 1) ? 2 : 1;
    } else {
        dischargeGear = (dischargeGear == 1) ? 2 : 1;
    }
    lastEvent = EVENT_MANUAL_GEAR;
    applyPowerControl();  // 统一通过互锁逻辑更新硬件
    updateOLED();
}

void longPressKey2() {
    Serial.println("Key2 长按释放触发");
    if (currentState == STATE_STOP) {
        currentMode = (currentMode == MODE_CHARGE) ? MODE_DISCHARGE : MODE_CHARGE;
        lastEvent = EVENT_MANUAL_MODE;
        Serial.println("-> 模式已切换");
        updateOLED();
    } else {
        Serial.println("-> 运行中，模式切换被锁定！");
    }
}

// ================= Arduino 主程序 =================
void setup() {
    Serial.begin(115200);
    Serial.println("系统启动中...");

    // ==============================================
    // 第一阶段：电源稳定 + I2C总线初始化（最关键）
    // ==============================================
    // 冷上电时给电源足够的稳定时间（不要超过1秒，太长会导致OLED进入休眠）
    delay(300);

    // 1. 手动初始化I2C总线，强制设置SDA/SCL为输出高电平
    // 解决ESP32 GPIO上电浮空导致OLED POR失败的问题
    pinMode(21, OUTPUT);
    pinMode(22, OUTPUT);
    digitalWrite(21, HIGH);
    digitalWrite(22, HIGH);
    delay(10); // 保持高电平10ms，满足SSD1306 POR要求
    
    // 初始化 I2C (OLED + BQ27220 共享总线)
    Wire.begin(21, 22);

    // TODO: 断电后上电屏幕有概率不亮，按RESET才能正常启动。
    // 疑似 I2C 上电时序问题 — SSD1306 POR 要求在 VCC 稳定后等待 >100ms，
    // 当前仅在 SDA/SCL 拉高后等了 10ms。可尝试在 u8g2.begin() 之前加
    // delay(200) 让 OLED 内部复位电路有足够时间完成上电初始化。
    // 初始化 OLED
    u8g2.begin();
    u8g2.enableUTF8Print();

    // 初始化充放电控制引脚
    pinMode(PIN_CHARGE_EN, OUTPUT);
    pinMode(PIN_CHARGE_CURRENT, OUTPUT);
    pinMode(PIN_DISCHARGE_EN, OUTPUT);
    pinMode(PIN_DISCHARGE_CURRENT, OUTPUT);
    digitalWrite(PIN_CHARGE_EN, LOW);
    digitalWrite(PIN_CHARGE_CURRENT, LOW);
    digitalWrite(PIN_DISCHARGE_EN, LOW);
    digitalWrite(PIN_DISCHARGE_CURRENT, LOW);

    // 初始化 BQ27220 (与 OLED 共享 I2C, Wire 已初始化)
    bq27220_ok = fuelGauge.begin(Wire);
    if (bq27220_ok) {
        Serial.println("BQ27220 检测成功");

        // ====== 配置电池容量为1500mAh (仅需烧录一次, 写完后可注释掉) ======
        fuelGauge.unseal();
        fuelGauge.fullAccess();

        fuelGauge.beginConfigUpdate();                   // ENTER_CFG_UPDATE
        for (int i = 0; i < 50; i++) {
            uint16_t opStatus;
            if (fuelGauge.readWord(0x3A, opStatus) && (opStatus & 0x04)) break;
            delay(10);
        }

        fuelGauge.writeDataMemoryU16(0x929F, 1500);      // Design Capacity
        fuelGauge.writeDataMemoryU16(0x929D, 1500);      // Full Charge Capacity (FCC)

        fuelGauge.endConfigUpdate(true);                 // EXIT_CFG_UPDATE_REINIT
        for (int i = 0; i < 50; i++) {
            uint16_t opStatus;
            if (fuelGauge.readWord(0x3A, opStatus) && !(opStatus & 0x04)) break;
            delay(10);
        }

        fuelGauge.seal();
        Serial.println("BQ27220 容量已配置: DesignCap=FCC=1500mAh");
        // TODO: 当前电池容量硬编码为1500mAh。后续需支持:
        // 1. 通过按键组合进入设置模式修改容量值
        // 2. 或通过蓝牙/串口命令动态配置
        // 3. 容量值写入BQ27220 Data Memory后永久保存，无需每次上电重写
        // ====== 容量配置结束 ======
    } else {
        Serial.println("BQ27220 未检测到！");
    }

    // 初始化 SD 卡
    pinMode(PIN_SD_DET, INPUT_PULLUP);
    if (digitalRead(PIN_SD_DET) == LOW) {
        sd_card_ok = initSDCard();
    } else {
        Serial.println("SD卡未插入");
    }

    // 配置按键
    button1.setPressMs(2000);
    button2.setPressMs(2000);
    button1.attachClick(clickKey1);
    button1.attachLongPressStart(longPressKey1);
    button2.attachClick(clickKey2);
    button2.attachLongPressStart(longPressKey2);

    // 初始显示
    lastDisplayUpdate = millis();
    updateOLED();
    Serial.println("初始化完成！");
}

void loop() {
    unsigned long now = millis();

    // 1. 按键扫描 (高频)
    button1.tick();
    button2.tick();

    // 2. 传感器读取 (每1000ms)
    if (now - lastSensorRead >= 1000) {
        lastSensorRead = now;
        readBatteryData();
    }

    // 3. 显示刷新 (每500ms)
    if (now - lastDisplayUpdate >= 500) {
        lastDisplayUpdate = now;
        updateOLED();
    }

    // 4. SD 卡日志 (每5000ms, 仅运行态)
    if ((currentState == STATE_CHARGE_RUN || currentState == STATE_DISCHARGE_RUN)
        && now - lastSDLog >= 5000) {
        lastSDLog = now;
        logToSDCard();
    }
    

    // 5. SD 卡热插拔检测 (每2000ms)
    if (now - lastSDCheck >= 2000) {
        lastSDCheck = now;
        bool cardPresent = (digitalRead(PIN_SD_DET) == LOW);
        if (cardPresent && !sd_card_ok) {
            sd_card_ok = initSDCard();
        } else if (!cardPresent && sd_card_ok) {
            sd_card_ok = false;
            Serial.println("SD卡已拔出");
        }
    }

    // 6. BQ27220 恢复尝试 (每30000ms, 仅故障时)
    if (!bq27220_ok && now - lastBQRecovery >= 30000) {
        lastBQRecovery = now;
        bq27220_ok = fuelGauge.begin(Wire);
        if (bq27220_ok) {
            bqFailCount = 0;
            Serial.println("BQ27220 已恢复！");
        }
    }
}
