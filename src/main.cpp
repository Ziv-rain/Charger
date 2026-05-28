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

// ================= BQ27220 传感器读取 =================
void readBatteryData() {
    if (!bq27220_ok) return;

    int soc = fuelGauge.readStateOfChargePercent();
    int mv  = fuelGauge.readVoltageMillivolts();
    int ma  = fuelGauge.readCurrentMilliamps();

    if (soc < 0 || mv < 0) {
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
    // 温度范围验证：-40°C到85°C（BQ27220工作范围）
    batteryTemp = (isnan(tempC) || tempC < -40.0f || tempC > 85.0f) ? NAN : tempC;

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

// ================= 辅助函数：写入Data Memory =================
// 按照技术手册第65页的示例实现
bool writeDataMemoryRaw(uint16_t address, uint16_t value) {
    uint8_t devAddr = 0x55;  // BQ27220 I2C地址

    // Step 0: 发送BlockDataControl()命令 (0x61 = 0x00)
    Wire.beginTransmission(devAddr);
    Wire.write(0x61);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) { Serial.println("BlockDataControl失败"); return false; }
    delay(2);

    // Step 5-6: 写入地址到 0x3E/0x3F
    Wire.beginTransmission(devAddr);
    Wire.write(0x3E);
    Wire.write((uint8_t)(address & 0xFF));
    if (Wire.endTransmission() != 0) { Serial.println("写0x3E失败"); return false; }

    Wire.beginTransmission(devAddr);
    Wire.write(0x3F);
    Wire.write((uint8_t)((address >> 8) & 0xFF));
    if (Wire.endTransmission() != 0) { Serial.println("写0x3F失败"); return false; }

    delay(2);

    // Step 7: 读取旧的checksum
    Wire.beginTransmission(devAddr);
    Wire.write(0x60);
    Wire.endTransmission(false);
    Wire.requestFrom((int)devAddr, 1);
    uint8_t oldChksum = Wire.read();

    // Step 8: 读取data length
    Wire.beginTransmission(devAddr);
    Wire.write(0x61);
    Wire.endTransmission(false);
    Wire.requestFrom((int)devAddr, 1);
    uint8_t dataLen = Wire.read();

    // Step 9: 读取旧的数据值
    Wire.beginTransmission(devAddr);
    Wire.write(0x40);
    Wire.endTransmission(false);
    Wire.requestFrom((int)devAddr, 2);
    uint8_t oldLSB = Wire.read();
    uint8_t oldMSB = Wire.read();

    // Step 10: 写入新数据
    Wire.beginTransmission(devAddr);
    Wire.write(0x40);
    Wire.write((uint8_t)(value & 0xFF));        // 新的LSB
    Wire.write((uint8_t)((value >> 8) & 0xFF));  // 新的MSB
    if (Wire.endTransmission() != 0) { Serial.println("写0x40失败"); return false; }

    // Step 11: 计算新的checksum (替换法)
    uint8_t temp = (255 - oldChksum - oldMSB - oldLSB) & 0xFF;
    uint8_t newChksum = (255 - ((temp + (uint8_t)((value >> 8) & 0xFF) + (uint8_t)(value & 0xFF)) & 0xFF)) & 0xFF;

    // Step 12: 写入新的checksum
    Wire.beginTransmission(devAddr);
    Wire.write(0x60);
    Wire.write(newChksum);
    if (Wire.endTransmission() != 0) { Serial.println("写checksum失败"); return false; }

    // Step 13: 写入data length (触发实际写入)
    Wire.beginTransmission(devAddr);
    Wire.write(0x61);
    Wire.write(dataLen);
    if (Wire.endTransmission() != 0) { Serial.println("写length失败"); return false; }

    delay(4);
    Serial.printf("  写入成功: 地址=0x%04X, 值=%d, 旧chk=0x%02X, 新chk=0x%02X, len=%d\n",
                  address, value, oldChksum, newChksum, dataLen);
    return true;
}

// ================= 配置电池容量 1500mAh =================
// 严格按照技术手册第65页示例实现
void configureBattery1500mAh() {
    Serial.println("配置电池容量: 1500mAh...");

    // Step 1: Unseal (一次性写入两个字节)
    Wire.beginTransmission(0x55);
    Wire.write(0x00);  // Control()寄存器
    Wire.write(0x14);  // LSB of 0x0414
    Wire.write(0x04);  // MSB of 0x0414
    Wire.endTransmission();

    Wire.beginTransmission(0x55);
    Wire.write(0x00);
    Wire.write(0x72);  // LSB of 0x3672
    Wire.write(0x36);  // MSB of 0x3672
    Wire.endTransmission();
    delay(100);
    Serial.println("Unseal 完成");

    // Step 2: Full Access
    Wire.beginTransmission(0x55);
    Wire.write(0x00);
    Wire.write(0xFF);  // LSB of 0xFFFF
    Wire.write(0xFF);  // MSB of 0xFFFF
    Wire.endTransmission();

    Wire.beginTransmission(0x55);
    Wire.write(0x00);
    Wire.write(0xFF);
    Wire.write(0xFF);
    Wire.endTransmission();
    delay(100);
    Serial.println("Full Access 完成");

    // Step 3: Enter Config Update (0x0090)
    Wire.beginTransmission(0x55);
    Wire.write(0x00);  // Control()寄存器
    Wire.write(0x90);  // LSB of 0x0090
    Wire.write(0x00);  // MSB of 0x0090
    Wire.endTransmission();
    Serial.println("发送Config Update命令...");
    delay(2000);  // 等待进入Config Update模式

    // Step 4: 确认CFGUPDATE模式
    Wire.beginTransmission(0x55);
    Wire.write(0x3A);  // OperationStatus寄存器
    Wire.endTransmission(false);
    Wire.requestFrom((int)0x55, 2);
    uint8_t osLow = Wire.read();
    uint8_t osHigh = Wire.read();
    uint16_t opStatus = osLow | (osHigh << 8);
    Serial.printf("OperationStatus: 0x%04X (bit2=%d)\n", opStatus, (opStatus >> 2) & 1);

    if (!((opStatus >> 2) & 1)) {
        Serial.println("错误：未进入Config Update模式！");
        return;
    }

    // 辅助函数：写入Data Memory (替换法)
    auto writeDM = [](uint16_t addr, uint16_t value) -> bool {
        // 设置地址
        Wire.beginTransmission(0x55);
        Wire.write(0x3E);
        Wire.write((uint8_t)(addr & 0xFF));
        Wire.endTransmission();

        Wire.beginTransmission(0x55);
        Wire.write(0x3F);
        Wire.write((uint8_t)((addr >> 8) & 0xFF));
        Wire.endTransmission();
        delay(2);

        // 读取旧的checksum和length
        Wire.beginTransmission(0x55);
        Wire.write(0x60);
        Wire.endTransmission(false);
        Wire.requestFrom((int)0x55, 2);
        uint8_t oldChksum = Wire.read();
        uint8_t dataLen = Wire.read();

        // 读取旧数据
        Wire.beginTransmission(0x55);
        Wire.write(0x40);
        Wire.endTransmission(false);
        Wire.requestFrom((int)0x55, 2);
        uint8_t oldLSB = Wire.read();
        uint8_t oldMSB = Wire.read();

        uint8_t newLSB = (uint8_t)(value & 0xFF);
        uint8_t newMSB = (uint8_t)((value >> 8) & 0xFF);

        Serial.printf("    旧值=%d, 新值=%d, 旧chk=0x%02X, len=%d\n",
                      oldLSB | (oldMSB << 8), value, oldChksum, dataLen);

        // 写入新数据
        Wire.beginTransmission(0x55);
        Wire.write(0x40);
        Wire.write(newLSB);
        Wire.write(newMSB);
        Wire.endTransmission();

        // 计算新checksum (替换法)
        uint8_t temp = (255 - oldChksum - oldMSB - oldLSB) & 0xFF;
        uint8_t newChksum = (255 - ((temp + newMSB + newLSB) & 0xFF)) & 0xFF;

        // 写入新checksum
        Wire.beginTransmission(0x55);
        Wire.write(0x60);
        Wire.write(newChksum);
        Wire.endTransmission();

        // 写入length (触发实际写入)
        Wire.beginTransmission(0x55);
        Wire.write(0x61);
        Wire.write(dataLen);
        Wire.endTransmission();
        delay(4);

        // 验证写入
        Wire.beginTransmission(0x55);
        Wire.write(0x40);
        Wire.endTransmission(false);
        Wire.requestFrom((int)0x55, 2);
        uint8_t verifyLSB = Wire.read();
        uint8_t verifyMSB = Wire.read();
        uint16_t verifyVal = verifyLSB | (verifyMSB << 8);

        Serial.printf("    验证: %d mAh\n", verifyVal);
        return verifyVal == value;
    };

    // Step 5: 写入所有CEDV参数（1500mAh电池估算值）
    Serial.println("写入CEDV参数...");

    // 基本容量
    writeDM(0x929F, 1500);  // Design Capacity
    writeDM(0x929D, 1500);  // Full Charge Capacity
    writeDM(0x92A3, 3700);  // Design Voltage (标称电压)

    // CEDV算法参数（按比例缩放）
    // 3000mAh默认值 → 1500mAh估算值
    writeDM(0x92A7, 3743);  // EMF (电动势，与容量无关)
    writeDM(0x92A9, 75);    // C0 (电容参数，按比例缩放 149 * 0.5)
    writeDM(0x92AB, 1734);  // R0 (内阻，小电池内阻更大 867 * 2)
    writeDM(0x92AD, 4030);  // T0 (温度系数，与容量无关)
    writeDM(0x92AF, 632);   // R1 (内阻系数 316 * 2)
    writeDM(0x92B1, 9);     // TC (温度系数，与容量无关)
    writeDM(0x92B2, 0);     // C1 (通常为0)

    // EDV阈值（放电截止电压）
    writeDM(0x92B4, 3000);  // EDV0 (3.0V)
    writeDM(0x92B7, 3300);  // EDV1 (3.3V)
    writeDM(0x92BA, 3500);  // EDV2 (3.5V)

    // 电压-DOD曲线（11个点，按比例缩放）
    // 默认值是3000mAh电池的曲线
    writeDM(0x92BD, 4173);  // Voltage 0% DOD
    writeDM(0x92BF, 4043);  // Voltage 10% DOD
    writeDM(0x92C1, 3925);  // Voltage 20% DOD
    writeDM(0x92C3, 3821);  // Voltage 30% DOD
    writeDM(0x92C5, 3725);  // Voltage 40% DOD
    writeDM(0x92C7, 3656);  // Voltage 50% DOD
    writeDM(0x92C9, 3619);  // Voltage 60% DOD
    writeDM(0x92CB, 3582);  // Voltage 70% DOD
    writeDM(0x92CD, 3530);  // Voltage 80% DOD
    writeDM(0x92CF, 3439);  // Voltage 90% DOD
    writeDM(0x92D1, 2713);  // Voltage 100% DOD

    Serial.println("CEDV参数写入完成");

    // Step 7: 退出Config Update并重新初始化
    Wire.beginTransmission(0x55);
    Wire.write(0x00);
    Wire.write(0x91);  // LSB of 0x0091 (EXIT_CFG_UPDATE_REINIT)
    Wire.write(0x00);  // MSB
    Wire.endTransmission();
    Serial.println("退出Config Update...");
    delay(2000);

    // Step 8: Reset
    Wire.beginTransmission(0x55);
    Wire.write(0x00);
    Wire.write(0x41);  // LSB of 0x0041 (RESET)
    Wire.write(0x00);  // MSB
    Wire.endTransmission();
    Serial.println("Reset 完成");
    delay(2000);

    // Step 9: Seal
    Wire.beginTransmission(0x55);
    Wire.write(0x00);
    Wire.write(0x30);  // LSB of 0x0030 (SEALED)
    Wire.write(0x00);  // MSB
    Wire.endTransmission();
    Serial.println("Seal 完成");
    delay(1000);

    // 验证
    uint16_t designCap;
    fuelGauge.readWord(0x3C, designCap);
    Serial.printf("验证 Command 0x3C: %d mAh\n", designCap);

    int fcc = fuelGauge.readFullChargeCapacitymAh();
    Serial.printf("验证 FCC: %d mAh\n", fcc);

    int rm = fuelGauge.readRemainingCapacitymAh();
    Serial.printf("验证 RM: %d mAh\n", rm);

    int soc = fuelGauge.readStateOfChargePercent();
    Serial.printf("验证 SOC: %d%%\n", soc);
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
        drawProgressBar(55, "Config Battery...");
        delay(500);
        configureBattery1500mAh();
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
