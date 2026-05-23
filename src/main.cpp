#include "main.h"
#include "display.h"
#include "sdcard.h"

// ================= 全局对象定义 =================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
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
bool bt_connected = false;

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
        digitalWrite(PIN_CHARGE_CURRENT, (chargeGear == 2) ? HIGH : LOW);
        delayMicroseconds(500);
        digitalWrite(PIN_CHARGE_EN, HIGH);
    } else if (dischargeOn) {
        digitalWrite(PIN_DISCHARGE_EN, LOW);
        digitalWrite(PIN_CHARGE_EN, LOW);
        digitalWrite(PIN_DISCHARGE_CURRENT, (dischargeGear == 2) ? HIGH : LOW);
        delayMicroseconds(500);
        digitalWrite(PIN_DISCHARGE_EN, HIGH);
    } else {
        digitalWrite(PIN_CHARGE_EN, LOW);
        digitalWrite(PIN_DISCHARGE_EN, LOW);
    }
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

    float tempC = fuelGauge.readTemperatureCelsius();
    batteryTemp = isnan(tempC) ? NAN : tempC;

    int avgMa = fuelGauge.readAverageCurrentMilliamps();
    batteryAvgCurrent = (avgMa == INT16_MIN) ? 0 : avgMa;

    batteryRemainCap = fuelGauge.readRemainingCapacitymAh();
    batteryFullCap   = fuelGauge.readFullChargeCapacitymAh();
    batteryDesignCap = fuelGauge.readDesignCapacitymAh();

    batteryCycleCount = fuelGauge.readCycleCount();
    batterySOH = fuelGauge.readStateOfHealthPercent();

    batteryTTE = fuelGauge.readTimeToEmptyMinutes();
    batteryTTF = fuelGauge.readTimeToFullMinutes();

    fuelGauge.readBatteryStatus(batteryStatus);
    fuelGauge.readOperationStatus(operationStatus);
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
    lastEvent = fromStop ? EVENT_MANUAL_START : EVENT_MANUAL_STOP;
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

// ================= Arduino 主程序 =================
void setup() {
    Serial.begin(115200);
    Serial.println("系统启动中...");

    delay(300);
    pinMode(21, OUTPUT);
    pinMode(22, OUTPUT);
    digitalWrite(21, HIGH);
    digitalWrite(22, HIGH);
    delay(10);

    Wire.begin(21, 22);

    u8g2.begin();
    u8g2.enableUTF8Print();

    pinMode(PIN_CHARGE_EN, OUTPUT);
    pinMode(PIN_CHARGE_CURRENT, OUTPUT);
    pinMode(PIN_DISCHARGE_EN, OUTPUT);
    pinMode(PIN_DISCHARGE_CURRENT, OUTPUT);
    digitalWrite(PIN_CHARGE_EN, LOW);
    digitalWrite(PIN_CHARGE_CURRENT, LOW);
    digitalWrite(PIN_DISCHARGE_EN, LOW);
    digitalWrite(PIN_DISCHARGE_CURRENT, LOW);

    bq27220_ok = fuelGauge.begin(Wire);
    if (bq27220_ok) {
        Serial.println("BQ27220 检测成功");

        // ====== 配置电池容量为1500mAh (仅需烧录一次, 写完后可注释掉) ======
        fuelGauge.unseal();
        fuelGauge.fullAccess();

        fuelGauge.beginConfigUpdate();
        for (int i = 0; i < 50; i++) {
            uint16_t opStatus;
            if (fuelGauge.readWord(0x3A, opStatus) && (opStatus & 0x04)) break;
            delay(10);
        }

        fuelGauge.writeDataMemoryU16(0x929F, 1500);
        fuelGauge.writeDataMemoryU16(0x929D, 1500);

        fuelGauge.endConfigUpdate(true);
        for (int i = 0; i < 50; i++) {
            uint16_t opStatus;
            if (fuelGauge.readWord(0x3A, opStatus) && !(opStatus & 0x04)) break;
            delay(10);
        }

        fuelGauge.seal();
        Serial.println("BQ27220 容量已配置: DesignCap=FCC=1500mAh");
        // ====== 容量配置结束 ======
    } else {
        Serial.println("BQ27220 未检测到！");
    }

    pinMode(PIN_SD_DET, INPUT_PULLUP);
    if (digitalRead(PIN_SD_DET) == LOW) {
        sd_card_ok = initSDCard();
    } else {
        Serial.println("SD卡未插入");
    }

    button1.setPressMs(2000);
    button2.setPressMs(2000);
    button1.attachClick(clickKey1);
    button1.attachLongPressStart(longPressKey1);
    button2.attachClick(clickKey2);
    button2.attachLongPressStart(longPressKey2);

    lastDisplayUpdate = millis();
    updateOLED();
    Serial.println("初始化完成！");
}

void loop() {
    unsigned long now = millis();

    button1.tick();
    button2.tick();

    if (now - lastSensorRead >= 1000) {
        lastSensorRead = now;
        readBatteryData();
    }

    if (now - lastDisplayUpdate >= 500) {
        lastDisplayUpdate = now;
        updateOLED();
    }

    if ((currentState == STATE_CHARGE_RUN || currentState == STATE_DISCHARGE_RUN)
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
