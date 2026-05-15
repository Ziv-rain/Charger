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

// ================= 全局变量 =================
SystemState currentState = STATE_STOP;
WorkMode currentMode = MODE_CHARGE;
int chargeGear = 1;
int dischargeGear = 1;

// 外设状态
bool bq27220_ok = false;
bool sd_card_ok = false;

// 电池数据缓存
int batterySOC = -1;
int batteryVoltage = -1;
int batteryCurrent = 0;

// 定时器
unsigned long lastDisplayUpdate = 0;
unsigned long lastSensorRead = 0;
unsigned long lastSDLog = 0;
unsigned long lastSDCheck = 0;
unsigned long lastBQRecovery = 0;

// BQ27220 故障计数
int bqFailCount = 0;

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
    if (!SD.begin(PIN_SD_CS)) {
        Serial.println("SD卡初始化失败！");
        return false;
    }
    File logFile = SD.open("/charger.csv", FILE_APPEND);
    if (logFile) {
        logFile.println("timestamp_ms,state,gear,SOC_pct,voltage_mV,current_mA");
        logFile.close();
    }
    Serial.println("SD卡初始化成功");
    return true;
}

void logToSDCard() {
    if (!sd_card_ok) return;
    File logFile = SD.open("/charger.csv", FILE_APPEND);
    if (!logFile) {
        sd_card_ok = false;
        return;
    }
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
        logFile.println(batteryCurrent);
    } else {
        logFile.println(",,,");
    }
    logFile.close();
}

// ================= BQ27220 传感器读取 =================
void readBatteryData() {
    if (!bq27220_ok) return;

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
}

// ================= OLED 显示刷新 =================
void updateOLED() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
    u8g2.setCursor(0, 15);

    String displayStr = "";

    switch (currentState) {
        case STATE_STOP:
            displayStr += "停止 | ";
            displayStr += (currentMode == MODE_CHARGE) ? "充电模式 | " : "放电模式 | ";
            displayStr += String((currentMode == MODE_CHARGE) ? chargeGear : dischargeGear) + "档";
            break;
        case STATE_CHARGE_RUN:
            displayStr += "充电运行 | " + String(chargeGear) + "档";
            break;
        case STATE_CHARGE_PAUSE:
            displayStr += "充电暂停 | " + String(chargeGear) + "档";
            break;
        case STATE_DISCHARGE_RUN:
            displayStr += "放电运行 | " + String(dischargeGear) + "档";
            break;
        case STATE_DISCHARGE_PAUSE:
            displayStr += "放电暂停 | " + String(dischargeGear) + "档";
            break;
    }
    u8g2.print(displayStr.c_str());

    // 右上角 SD 卡状态
    u8g2.setCursor(110, 15);
    u8g2.print(sd_card_ok ? "SD" : "--");

    // 第二行：SOC
    u8g2.setCursor(0, 35);
    if (bq27220_ok && batterySOC >= 0) {
        u8g2.print("SOC: " + String(batterySOC) + "%");
    } else if (!bq27220_ok) {
        u8g2.print("SOC: --% [ERR]");
    } else {
        u8g2.print("SOC: --%");
    }

    // 第三行：电压和电流
    u8g2.setCursor(0, 55);
    if (bq27220_ok && batteryVoltage >= 0) {
        u8g2.print("V: " + String(batteryVoltage / 1000.0, 1) + " V  ");
        u8g2.print("I: " + String(batteryCurrent) + " mA");
    } else if (!bq27220_ok) {
        u8g2.print("V: --.- V  I: --- mA");
    } else {
        u8g2.print("V: --.- V  I: --- mA");
    }

    u8g2.sendBuffer();
}

// ================= 按键事件回调 =================

void clickKey1() {
    Serial.println("Key1 短按触发");
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
    applyPowerControl();
    if (sd_card_ok) logToSDCard();
    updateOLED();
}

void longPressKey1() {
    Serial.println("Key1 长按释放触发 -> 紧急停止");
    if (currentState != STATE_STOP) {
        currentState = STATE_STOP;
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
    applyPowerControl();  // 统一通过互锁逻辑更新硬件
    updateOLED();
}

void longPressKey2() {
    Serial.println("Key2 长按释放触发");
    if (currentState == STATE_STOP) {
        currentMode = (currentMode == MODE_CHARGE) ? MODE_DISCHARGE : MODE_CHARGE;
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

    // 初始化 I2C (OLED + BQ27220 共享总线)
    Wire.begin(21, 22);

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
