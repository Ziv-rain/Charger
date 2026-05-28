#include "main.h"
#include "display.h"
#include "sdcard.h"
#include "ble_comm.h"

// ================= 全局对象定义 =================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);
OneButton button1(PIN_KEY1, true, false);
OneButton button2(PIN_KEY2, true, false);
BQ27220 fuelGauge;

// ================= 全局状态变量 =================
SystemState currentState = STATE_STOP;
WorkMode currentMode = MODE_CHARGE;
int chargeGear = 1;
int dischargeGear = 1;

// ================= 外设状态 =================
bool bq27220_ok = false;
bool sd_card_ok = false;

// ================= 电池数据缓存 =================
int batterySOC = -1;
int batteryVoltage = -1;
int batteryCurrent = 0;
float batteryTemp = NAN;
int batteryAvgCurrent = 0;
int batteryRemainCap = -1;
int batteryFullCap = -1;
int batteryDesignCap = -1;
int batteryCycleCount = -1;
int batterySOH = -1;
int batteryTTE = -1;
int batteryTTF = -1;
uint16_t batteryStatus = 0;
uint16_t operationStatus = 0;

// ================= 定时器 =================
unsigned long lastDisplayUpdate = 0;
unsigned long lastSensorRead = 0;
unsigned long lastSDLog = 0;
unsigned long lastSDCheck = 0;
unsigned long lastBQRecovery = 0;

// ================= 故障计数 =================
int bqFailCount = 0;
int sdFailCount = 0;

// ================= 日志文件名 =================
char logFilename[32] = "/charger.csv";

// ================= 事件标记 =================
uint8_t lastEvent = EVENT_NONE;

// ================= 自定义SOC计算 =================
int customSOC = -1;
float remainingCapacityMah = DESIGN_CAPACITY_MAH;
unsigned long lastSocCalcTime = 0;

// ================= 充放电硬件控制 (互锁) =================
void applyPowerControl() {
    bool chargeOn = (currentState == STATE_CHARGE_RUN);
    bool dischargeOn = (currentState == STATE_DISCHARGE_RUN);

    if (chargeOn && dischargeOn) {
        digitalWrite(PIN_CHARGE_EN, LOW);
        digitalWrite(PIN_DISCHARGE_EN, LOW);
        return;
    }

    if (chargeOn) {
        digitalWrite(PIN_CHARGE_EN, LOW);
        digitalWrite(PIN_DISCHARGE_EN, LOW);
        delay(10);
        digitalWrite(PIN_CHARGE_CURRENT, (chargeGear == 2) ? HIGH : LOW);
        delay(5);
        digitalWrite(PIN_CHARGE_EN, HIGH);
    } else if (dischargeOn) {
        digitalWrite(PIN_DISCHARGE_EN, LOW);
        digitalWrite(PIN_CHARGE_EN, LOW);
        delay(10);
        digitalWrite(PIN_DISCHARGE_CURRENT, (dischargeGear == 2) ? HIGH : LOW);
        delay(5);
        digitalWrite(PIN_DISCHARGE_EN, HIGH);
    } else {
        digitalWrite(PIN_CHARGE_EN, LOW);
        digitalWrite(PIN_DISCHARGE_EN, LOW);
    }
}

// ================= 电压转SOC查表 =================
int voltageToSOC(int mv) {
    // 1500mAh锂电池电压-SOC对应关系
    if (mv >= 4200) return 100;
    if (mv >= 4000) return 85 + (mv - 4000) * 15 / 200;
    if (mv >= 3800) return 60 + (mv - 3800) * 25 / 200;
    if (mv >= 3700) return 50 + (mv - 3700) * 10 / 100;
    if (mv >= 3600) return 35 + (mv - 3600) * 15 / 100;
    if (mv >= 3500) return 20 + (mv - 3500) * 15 / 100;
    if (mv >= 3400) return 10 + (mv - 3400) * 10 / 100;
    if (mv >= 3300) return 5 + (mv - 3300) * 5 / 100;
    if (mv >= 3000) return (mv - 3000) * 5 / 300;
    return 0;
}

// ================= 自定义SOC计算 =================
void calculateCustomSOC() {
    unsigned long now = millis();

    // 首次读取：从BQ27220初始化
    if (lastSocCalcTime == 0) {
        int bq27220_soc = fuelGauge.readStateOfChargePercent();
        if (bq27220_soc >= 0) {
            customSOC = bq27220_soc;
            remainingCapacityMah = (DESIGN_CAPACITY_MAH * bq27220_soc) / 100;
            Serial.printf("SOC初始化: BQ27220_SOC=%d%%, RM=%dmAh\n",
                          bq27220_soc, remainingCapacityMah);
        } else {
            customSOC = voltageToSOC(batteryVoltage);
            remainingCapacityMah = (DESIGN_CAPACITY_MAH * customSOC) / 100;
            Serial.printf("SOC初始化: BQ27220失败, 电压法=%d%%\n", customSOC);
        }
        lastSocCalcTime = now;
        return;
    }

    // 非运行状态：用BQ27220的SOC校准（无电流流动，BQ27220是准的）
    if (currentState == STATE_STOP || currentState == STATE_CHARGE_PAUSE || currentState == STATE_DISCHARGE_PAUSE) {
        int bq27220_soc = fuelGauge.readStateOfChargePercent();
        if (bq27220_soc >= 0) {
            customSOC = bq27220_soc;
            remainingCapacityMah = (DESIGN_CAPACITY_MAH * bq27220_soc) / 100;
        } else {
            customSOC = voltageToSOC(batteryVoltage);
            remainingCapacityMah = (DESIGN_CAPACITY_MAH * customSOC) / 100;
        }
        lastSocCalcTime = now;
    } else {
        // 充放电状态：库仑计数（BQ27220此时不准，不用它）
        float dt_hours = (now - lastSocCalcTime) / 3600000.0f;
        float delta_mah = batteryCurrent * dt_hours;
        remainingCapacityMah += delta_mah;
        lastSocCalcTime = now;

        if (remainingCapacityMah > DESIGN_CAPACITY_MAH)
            remainingCapacityMah = DESIGN_CAPACITY_MAH;
        if (remainingCapacityMah < 0)
            remainingCapacityMah = 0;

        customSOC = (remainingCapacityMah * 100) / DESIGN_CAPACITY_MAH;
    }

    if (customSOC > 100) customSOC = 100;
    if (customSOC < 0) customSOC = 0;

    batterySOC = customSOC;
    batteryRemainCap = remainingCapacityMah;
    batteryFullCap = DESIGN_CAPACITY_MAH;
    batteryDesignCap = DESIGN_CAPACITY_MAH;

    static float prevRM = -1;
    float rmDelta = (prevRM >= 0) ? (remainingCapacityMah - prevRM) : 0;
    float socDeltaFloat = rmDelta * 100.0f / DESIGN_CAPACITY_MAH;
    prevRM = remainingCapacityMah;

    // 计算自定义预计剩余时间（分钟）
    int etaMin = -1;
    if (currentState == STATE_DISCHARGE_RUN && batteryCurrent < 0) {
        etaMin = (int)(remainingCapacityMah / (-batteryCurrent) * 60);
    } else if (currentState == STATE_CHARGE_RUN && batteryCurrent > 0) {
        etaMin = (int)((DESIGN_CAPACITY_MAH - remainingCapacityMah) / batteryCurrent * 60);
    }

    Serial.printf("[SOC] V=%dmV I=%dmA RM=%.1fmAh SOC=%d%% dSOC=%+.3f%% "
                  "ETA自定义=%dm BQ_TTE=%dm BQ_TTF=%dm state=%d\n",
                  batteryVoltage, batteryCurrent, remainingCapacityMah,
                  customSOC, socDeltaFloat, etaMin, batteryTTE, batteryTTF,
                  currentState);
}

// ================= BQ27220 传感器读取 =================
void readBatteryData() {
    if (!bq27220_ok) return;

    int mv  = fuelGauge.readVoltageMillivolts();
    int ma  = fuelGauge.readCurrentMilliamps();

    if (mv < 0) {
        bqFailCount++;
        if (bqFailCount > 3) {
            bq27220_ok = false;
            Serial.println("BQ27220 通信故障！");
        }
        return;
    }
    bqFailCount = 0;
    batteryVoltage = mv;
    batteryCurrent = ma;

    float tempC = fuelGauge.readTemperatureCelsius();
    // 温度范围验证：-40°C到85°C（BQ27220工作范围）
    batteryTemp = (isnan(tempC) || tempC < -40.0f || tempC > 85.0f) ? NAN : tempC;

    int avgMa = fuelGauge.readAverageCurrentMilliamps();
    batteryAvgCurrent = (avgMa == INT16_MIN) ? 0 : avgMa;

    // 使用自定义SOC计算
    calculateCustomSOC();

    // 其他数据仍从BQ27220读取
    batteryCycleCount = fuelGauge.readCycleCount();
    batterySOH = fuelGauge.readStateOfHealthPercent();

    batteryTTE = fuelGauge.readTimeToEmptyMinutes();
    batteryTTF = fuelGauge.readTimeToFullMinutes();

    fuelGauge.readBatteryStatus(batteryStatus);
    fuelGauge.readOperationStatus(operationStatus);
}

// ================= 自动截止检测 =================
void checkAutoCutoff() {
    if (currentState == STATE_CHARGE_RUN) {
        if (batteryVoltage >= bleChargeCutoff) {
            currentState = STATE_STOP;
            lastEvent = EVENT_AUTO_CUTOFF_FULL;
            applyPowerControl();
            if (sd_card_ok) logToSDCard();
            updateOLED();
            Serial.printf("自动截止：充满 %dmV >= %dmV\n", batteryVoltage, bleChargeCutoff);
        }
    } else if (currentState == STATE_DISCHARGE_RUN) {
        if (batteryVoltage <= bleDischargeCutoff) {
            currentState = STATE_STOP;
            lastEvent = EVENT_AUTO_CUTOFF_EMPTY;
            applyPowerControl();
            if (sd_card_ok) logToSDCard();
            updateOLED();
            Serial.printf("自动截止：放空 %dmV <= %dmV\n", batteryVoltage, bleDischargeCutoff);
        }
    }
}

// ================= 按键事件回调 =================

void clickKey1() {
    Serial.println("Key1 短按触发");
    bool fromStop = (currentState == STATE_STOP);
    switch (currentState) {
        case STATE_STOP:
            currentState = (currentMode == MODE_CHARGE) ? STATE_CHARGE_RUN : STATE_DISCHARGE_RUN;
            break;
        case STATE_CHARGE_RUN:       currentState = STATE_CHARGE_PAUSE; break;
        case STATE_CHARGE_PAUSE:     currentState = STATE_CHARGE_RUN; break;
        case STATE_DISCHARGE_RUN:    currentState = STATE_DISCHARGE_PAUSE; break;
        case STATE_DISCHARGE_PAUSE:  currentState = STATE_DISCHARGE_RUN; break;
    }
    if (fromStop) {
        lastEvent = EVENT_MANUAL_START;
    } else if (currentState == STATE_CHARGE_PAUSE || currentState == STATE_DISCHARGE_PAUSE) {
        lastEvent = EVENT_MANUAL_PAUSE;
    } else {
        lastEvent = EVENT_MANUAL_RESUME;
    }
    applyPowerControl();
    if (sd_card_ok) logToSDCard();
    updateOLED();
}

void longPressKey1() {
    Serial.println("Key1 长按触发 -> 紧急停止");
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
    applyPowerControl();
    if (sd_card_ok) logToSDCard();
    updateOLED();
}

void longPressKey2() {
    Serial.println("Key2 长按触发");
    if (currentState == STATE_STOP) {
        currentMode = (currentMode == MODE_CHARGE) ? MODE_DISCHARGE : MODE_CHARGE;
        lastEvent = EVENT_MANUAL_MODE;
        Serial.println("-> 模式已切换");
        updateOLED();
    } else {
        Serial.println("-> 运行中，模式切换被锁定！");
    }
}

// ================= 进度条绘制 =================
void drawProgressBar(int percent, const char* status) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_7x13B_tf);

    // 标题
    u8g2.drawStr(24, 14, "INITIALIZING");

    // 进度条背景
    u8g2.drawFrame(10, 24, 108, 12);

    // 进度条填充
    int fillWidth = (percent * 104) / 100;
    u8g2.drawBox(12, 26, fillWidth, 8);

    // 百分比
    char percentStr[8];
    snprintf(percentStr, sizeof(percentStr), "%d%%", percent);
    u8g2.drawStr(52, 48, percentStr);

    // 状态文字
    u8g2.drawStr(10, 62, status);

    u8g2.sendBuffer();
}

// ================= Arduino 主程序 =================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("系统启动中...");

    // OLED初始化（u8g2会自动初始化I2C总线）
    u8g2.begin();
    u8g2.enableUTF8Print();
    drawProgressBar(10, "OLED Ready");

    // I2C时钟配置
    Wire.setClock(100000);  // 降低I2C速度到100kHz，提高稳定性

    // GPIO初始化
    drawProgressBar(35, "GPIO Init...");
    pinMode(PIN_CHARGE_EN, OUTPUT);
    pinMode(PIN_CHARGE_CURRENT, OUTPUT);
    pinMode(PIN_DISCHARGE_EN, OUTPUT);
    pinMode(PIN_DISCHARGE_CURRENT, OUTPUT);
    digitalWrite(PIN_CHARGE_EN, LOW);
    digitalWrite(PIN_CHARGE_CURRENT, LOW);
    digitalWrite(PIN_DISCHARGE_EN, LOW);
    digitalWrite(PIN_DISCHARGE_CURRENT, LOW);

    // BQ27220初始化
    drawProgressBar(50, "BQ27220 Detect...");
    delay(1000);  // 增加等待时间
    bq27220_ok = fuelGauge.begin(Wire, 0x55, -1, -1, 100000);  // 100kHz
    if (bq27220_ok) {
        Serial.println("BQ27220 检测成功");
        drawProgressBar(65, "BQ27220 Ready");
    } else {
        Serial.println("BQ27220 未检测到！");
        drawProgressBar(65, "BQ27220 Failed");
    }

    // SD卡初始化
    drawProgressBar(75, "SD Card Detect...");
    pinMode(PIN_SD_DET, INPUT_PULLUP);
    if (digitalRead(PIN_SD_DET) == LOW) {
        sd_card_ok = initSDCard();
        if (sd_card_ok) {
            drawProgressBar(85, "SD Card Ready");
        } else {
            drawProgressBar(85, "SD Card Failed");
        }
    } else {
        Serial.println("SD卡未插入");
        drawProgressBar(85, "No SD Card");
    }

    // 按钮初始化
    button1.setPressMs(1000);
    button2.setPressMs(1000);
    button1.attachClick(clickKey1);
    button1.attachLongPressStart(longPressKey1);
    button2.attachClick(clickKey2);
    button2.attachLongPressStart(longPressKey2);

    // BLE初始化
    drawProgressBar(90, "BLE Init...");
    initBLE();
    drawProgressBar(100, "Init Done");

    delay(500);
    lastDisplayUpdate = millis();
    updateOLED();

    Serial.println("初始化完成！");
}

void loop() {
    unsigned long now = millis();

    button1.tick();
    button2.tick();

    blePollCommand();

    if (now - lastSensorRead >= 1000) {
        lastSensorRead = now;
        readBatteryData();
        checkAutoCutoff();
    }

    {
        static unsigned long lastBLEReport = 0;
        if (bleDeviceConnected && now - lastBLEReport >= 2000) {
            lastBLEReport = now;
            bleUpdateAndNotify();
        }
    }

    if (now - lastDisplayUpdate >= 500) {
        lastDisplayUpdate = now;
        updateOLED();
    }

    if ((currentState == STATE_CHARGE_RUN || currentState == STATE_DISCHARGE_RUN ||
         currentState == STATE_CHARGE_PAUSE || currentState == STATE_DISCHARGE_PAUSE)
        && now - lastSDLog >= 5000) {
        lastSDLog = now;
        logToSDCard();
    }

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

    if (!bq27220_ok && now - lastBQRecovery >= 30000) {
        lastBQRecovery = now;
        bq27220_ok = fuelGauge.begin(Wire);
        if (bq27220_ok) {
            bqFailCount = 0;
            Serial.println("BQ27220 已恢复！");
        }
    }
}
