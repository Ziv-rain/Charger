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
unsigned long bq27220RunTime = 0;
bool bq27220NeedReset = false;
unsigned long lastStateChangeTime = 0;

// ================= 库仑计数优化 =================
float currentFilterBuf[CURRENT_FILTER_SIZE] = {0};
int currentFilterIdx = 0;
float coulombGain = COULOMB_GAIN_INIT;  // 动态增益系数

// ================= 自动校准 =================
unsigned long lastAutoCalibrateTime = 0;
bool autoCalibrating = false;
bool fastCalibrateMode = false;
int lastBq27220Soc = -1;
int socDeviationCount = 0;

// ================= 循环与能量统计 =================
int cycleNumber = 0;
int phaseType = 0;           // 0=休息, 1=充电, 2=放电
unsigned long phaseStartTime = 0;
float cumulativeMahIn = 0;
float cumulativeMahOut = 0;
float cycleMahIn = 0;
float cycleMahOut = 0;
float cumulativeWh = 0;
int lastVoltage = -1;
unsigned long lastDvDtTime = 0;
float dvDt = 0;
float estimatedIR = 0;

// ================= 阶段更新辅助函数 =================
void updatePhase(int newPhaseType) {
    phaseType = newPhaseType;
    phaseStartTime = millis();
}

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

// ================= BQ27220软重置 =================
void resetBQ27220() {
    if (!bq27220_ok) return;

    Serial.println("BQ27220 执行软重置...");

    // BQ27220软重置：Control() with RESET subcommand (0x0041)
    Wire.beginTransmission(0x55);
    Wire.write(0x00);  // Control register
    Wire.write(0x41);  // RESET subcommand low byte
    Wire.write(0x00);  // RESET subcommand high byte
    Wire.endTransmission();

    delay(3000);  // 等待重置完成，BQ27220需要时间重新学习

    // 重新初始化
    bq27220_ok = fuelGauge.begin(Wire, 0x55, -1, -1, 100000);
    if (bq27220_ok) {
        Serial.println("BQ27220 软重置成功，重新初始化完成");
        bq27220RunTime = 0;
        bq27220NeedReset = false;
    } else {
        Serial.println("BQ27220 软重置失败，尝试重新检测...");
        // 尝试重新初始化
        delay(500);
        bq27220_ok = fuelGauge.begin(Wire, 0x55, -1, -1, 100000);
        if (bq27220_ok) {
            Serial.println("BQ27220 重新检测成功");
            bq27220RunTime = 0;
            bq27220NeedReset = false;
        } else {
            Serial.println("BQ27220 恢复失败，将使用电压法");
        }
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
    if (autoCalibrating) return;  // 校准期间跳过
    unsigned long now = millis();
    static SystemState lastState = STATE_STOP;

    // 检测状态变化，记录充放电运行时间
    if (currentState != lastState) {
        // 从运行状态切换到暂停/停止状态
        if ((lastState == STATE_CHARGE_RUN || lastState == STATE_DISCHARGE_RUN) &&
            (currentState == STATE_STOP || currentState == STATE_CHARGE_PAUSE || currentState == STATE_DISCHARGE_PAUSE)) {
            bq27220RunTime += (now - lastStateChangeTime);
            Serial.printf("BQ27220 累计运行时间: %lu ms (%.1f 分钟)\n",
                          bq27220RunTime, bq27220RunTime / 60000.0f);

            // 检查是否需要重置
            if (bq27220RunTime >= BQ27220_RESET_THRESHOLD) {
                bq27220NeedReset = true;
                Serial.println("BQ27220 运行时间过长，标记需要重置");
            }
        }
        lastState = currentState;
        lastStateChangeTime = now;
    }

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
        // 状态切换后1.5秒内不校准SOC，等待电压稳定（充电→暂停时电压需要时间回落）
        if (now - lastStateChangeTime < 1500) {
            lastSocCalcTime = now;  // 更新时间，但不更新SOC
            return;
        }

        // 如果BQ27220需要重置，先执行软重置
        if (bq27220NeedReset) {
            resetBQ27220();
        }

        int bq27220_soc = fuelGauge.readStateOfChargePercent();
        int voltage_soc = voltageToSOC(batteryVoltage);

        // 对比BQ27220和电压法的SOC差异
        int soc_diff = abs(bq27220_soc - voltage_soc);
        Serial.printf("SOC对比: BQ27220=%d%%, 电压法=%d%%, 差异=%d%%\n",
                      bq27220_soc, voltage_soc, soc_diff);

        if (bq27220_soc >= 0) {
            if (soc_diff > 15) {
                // 差异过大，使用电压法
                Serial.println("BQ27220与电压法差异过大，使用电压法校准");
                customSOC = voltage_soc;
                remainingCapacityMah = (DESIGN_CAPACITY_MAH * voltage_soc) / 100;
                // 仅当BQ27220返回非零SOC时才标记需要重置（0%可能是刚重置还没初始化完）
                if (bq27220_soc > 0) {
                    bq27220NeedReset = true;
                }
            } else {
                customSOC = bq27220_soc;
                remainingCapacityMah = (DESIGN_CAPACITY_MAH * bq27220_soc) / 100;
            }
        } else {
            customSOC = voltage_soc;
            remainingCapacityMah = (DESIGN_CAPACITY_MAH * voltage_soc) / 100;
        }
        lastSocCalcTime = now;
    } else {
        // 充放电状态：库仑计数（BQ27220此时不准，不用它）
        float dt_hours = (now - lastSocCalcTime) / 3600000.0f;
        // 使用平均电流计算，增益系数让SOC变化更慢
        float calcCurrent = (batteryAvgCurrent != 0) ? batteryAvgCurrent : batteryCurrent;
        float delta_mah = calcCurrent * dt_hours * coulombGain;
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

// ================= 电流滤波 =================
float filterCurrent(float raw) {
    currentFilterBuf[currentFilterIdx] = raw;
    currentFilterIdx = (currentFilterIdx + 1) % CURRENT_FILTER_SIZE;

    float sum = 0;
    for (int i = 0; i < CURRENT_FILTER_SIZE; i++) {
        sum += currentFilterBuf[i];
    }
    return sum / CURRENT_FILTER_SIZE;
}

// ================= 自动校准 =================
void checkAutoCalibrate() {
    unsigned long now = millis();

    // 仅在充放电运行状态触发
    if (currentState != STATE_CHARGE_RUN && currentState != STATE_DISCHARGE_RUN) {
        lastAutoCalibrateTime = now;
        fastCalibrateMode = false;
        socDeviationCount = 0;
        return;
    }

    // 判断是否在充放电末尾，使用更短的校准间隔
    bool isNearEnd = false;
    if (currentState == STATE_CHARGE_RUN && (customSOC > 90 || batteryVoltage > 4100)) {
        isNearEnd = true;
    } else if (currentState == STATE_DISCHARGE_RUN && (customSOC < 20 || batteryVoltage < 3400)) {
        isNearEnd = true;
    }

    // 确定校准间隔
    unsigned long calibrateInterval;
    if (fastCalibrateMode) {
        calibrateInterval = AUTO_CALIBRATE_FAST_INTERVAL;  // 3分钟
    } else if (isNearEnd) {
        calibrateInterval = AUTO_CALIBRATE_FAST_INTERVAL;  // 3分钟（末尾）
    } else {
        calibrateInterval = AUTO_CALIBRATE_INTERVAL;       // 15分钟
    }

    // 检查是否到达校准间隔
    if (now - lastAutoCalibrateTime < calibrateInterval) return;

    Serial.println("自动校准: 开始...");
    autoCalibrating = true;

    // 1. 暂停充放电
    SystemState prevState = currentState;
    currentState = (currentMode == MODE_CHARGE) ? STATE_CHARGE_PAUSE : STATE_DISCHARGE_PAUSE;
    applyPowerControl();

    // 2. 等待电压稳定
    delay(AUTO_CALIBRATE_STABLE_TIME);

    // 3. 软重置BQ27220
    resetBQ27220();

    // 4. 读取校准后的SOC
    int bq27220_soc = fuelGauge.readStateOfChargePercent();
    if (bq27220_soc >= 0) {
        int old_soc = customSOC;

        // 检测方向是否正确
        bool directionOk = false;
        if (prevState == STATE_CHARGE_RUN || prevState == STATE_CHARGE_PAUSE) {
            directionOk = (bq27220_soc >= customSOC);
        } else if (prevState == STATE_DISCHARGE_RUN || prevState == STATE_DISCHARGE_PAUSE) {
            directionOk = (bq27220_soc <= customSOC);
        }

        if (directionOk) {
            // 方向正确，更新SOC
            customSOC = bq27220_soc;
            remainingCapacityMah = (DESIGN_CAPACITY_MAH * bq27220_soc) / 100;
            batterySOC = customSOC;
            batteryRemainCap = remainingCapacityMah;
            Serial.printf("自动校准: SOC %d%% -> %d%%\n", old_soc, customSOC);

            // 如果之前在快速校准模式，检查是否收敛
            if (fastCalibrateMode) {
                int diff = abs(bq27220_soc - old_soc);
                if (diff <= CALIBRATE_CONVERGE_THRESHOLD) {
                    // 收敛，恢复正常模式
                    fastCalibrateMode = false;
                    socDeviationCount = 0;
                    Serial.println("自动校准: 收敛，恢复正常模式");
                }
            }
        } else {
            // 方向不对，进入快速校准模式
            if (!fastCalibrateMode) {
                fastCalibrateMode = true;
                socDeviationCount = 0;
                Serial.println("自动校准: 方向不对，进入快速校准模式");
            }
            socDeviationCount++;
            lastBq27220Soc = bq27220_soc;

            // 调整增益系数
            if (socDeviationCount >= 3) {
                // 连续3次方向不对，调整增益
                float ratio = (float)bq27220_soc / (float)customSOC;
                float newGain = coulombGain * ratio;

                // 限制范围
                if (newGain < COULOMB_GAIN_MIN) newGain = COULOMB_GAIN_MIN;
                if (newGain > COULOMB_GAIN_MAX) newGain = COULOMB_GAIN_MAX;

                coulombGain = newGain;
                socDeviationCount = 0;
                Serial.printf("自动校准: 调整增益系数 -> %.2f\n", coulombGain);
            }

            Serial.printf("自动校准: 方向不对 BQ=%d%%, SOC=%d%%, 不更新\n",
                          bq27220_soc, customSOC);
        }
    } else {
        int voltage_soc = voltageToSOC(batteryVoltage);
        customSOC = voltage_soc;
        remainingCapacityMah = (DESIGN_CAPACITY_MAH * voltage_soc) / 100;
    }

    // 5. 恢复运行
    currentState = prevState;
    applyPowerControl();
    lastAutoCalibrateTime = now;
    lastSocCalcTime = now;
    autoCalibrating = false;

    Serial.println("自动校准: 完成");
}

// ================= BQ27220 传感器读取 =================
void readBatteryData() {
    if (!bq27220_ok) return;
    if (autoCalibrating) return;  // 校准期间跳过

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

    // 数据验证：检查电压是否合理（2.5V-4.5V范围）
    if (mv < 2500 || mv > 4500) {
        Serial.printf("BQ27220 电压异常: %dmV，跳过本次读数\n", mv);
        bqFailCount++;
        if (bqFailCount > 5) {
            bq27220_ok = false;
            Serial.println("BQ27220 连续异常，标记故障！");
        }
        return;
    }

    // 数据验证：检查电流是否合理（-5A到5A范围）
    if (ma < -5000 || ma > 5000) {
        Serial.printf("BQ27220 电流异常: %dmA，跳过本次读数\n", ma);
        bqFailCount++;
        if (bqFailCount > 5) {
            bq27220_ok = false;
            Serial.println("BQ27220 连续异常，标记故障！");
        }
        return;
    }

    // 电压变化检测：如果电压跳变超过200mV，可能是读取错误
    // 状态切换后1.5秒内不检测（充电→暂停时电压回落是正常的）
    unsigned long currentTime = millis();
    if (batteryVoltage > 0 && abs(mv - batteryVoltage) > 200 && (currentTime - lastStateChangeTime > 1500)) {
        Serial.printf("BQ27220 电压跳变: %dmV -> %dmV，可能是读取错误\n", batteryVoltage, mv);
        bqFailCount++;
        if (bqFailCount > 5) {
            bq27220_ok = false;
            Serial.println("BQ27220 连续跳变，标记故障！");
        }
        return;
    }

    bqFailCount = 0;
    batteryVoltage = mv;
    batteryCurrent = ma;  // 瞬时电流（OLED显示用）

    float tempC = fuelGauge.readTemperatureCelsius();
    // 温度范围验证：-40°C到85°C（BQ27220工作范围）
    batteryTemp = (isnan(tempC) || tempC < -40.0f || tempC > 85.0f) ? NAN : tempC;

    // 读取BQ27220平均电流（计算用）
    int avgMa = fuelGauge.readAverageCurrentMilliamps();
    batteryAvgCurrent = (avgMa == INT16_MIN) ? ma : avgMa;

    // 使用自定义SOC计算
    calculateCustomSOC();

    // 其他数据仍从BQ27220读取
    batteryCycleCount = fuelGauge.readCycleCount();
    batterySOH = fuelGauge.readStateOfHealthPercent();

    batteryTTE = fuelGauge.readTimeToEmptyMinutes();
    batteryTTF = fuelGauge.readTimeToFullMinutes();

    fuelGauge.readBatteryStatus(batteryStatus);
    fuelGauge.readOperationStatus(operationStatus);

    // 能量统计（库仑计数）
    {
        float dt_h = 1.0f / 3600.0f;  // 1秒对应的小时数
        if (batteryCurrent > 0) {
            // 充电
            float mah = batteryCurrent * dt_h;
            cumulativeMahIn += mah;
            cycleMahIn += mah;
            cumulativeWh += batteryVoltage * mah / 1000.0f;
        } else if (batteryCurrent < 0) {
            // 放电
            float mah = (-batteryCurrent) * dt_h;
            cumulativeMahOut += mah;
            cycleMahOut += mah;
        }
    }

    // dv/dt 计算（每秒更新）
    if (lastVoltage > 0 && lastDvDtTime > 0) {
        unsigned long now = millis();
        float dt_sec = (now - lastDvDtTime) / 1000.0f;
        if (dt_sec >= 0.5f) {
            dvDt = (batteryVoltage - lastVoltage) / dt_sec;
            lastVoltage = batteryVoltage;
            lastDvDtTime = now;
        }
    } else {
        lastVoltage = batteryVoltage;
        lastDvDtTime = millis();
    }

    // 内阻估算（电流变化时）
    if (abs(batteryCurrent) > 50 && abs(batteryAvgCurrent) > 50) {
        // 简单估算：ΔV / ΔI
        float deltaI = abs(batteryCurrent - batteryAvgCurrent);
        if (deltaI > 10) {
            estimatedIR = abs(dvDt) / deltaI * 1000.0f;  // mΩ
            if (estimatedIR > 1000) estimatedIR = 0;  // 异常值过滤
        }
    }
}

// ================= 自动截止检测 =================
void checkAutoCutoff() {
    if (currentState == STATE_CHARGE_RUN) {
        if (batteryVoltage >= bleChargeCutoff) {
            currentState = STATE_STOP;
            lastEvent = EVENT_AUTO_CUTOFF_FULL;
            updatePhase(0);  // 回到休息状态
            applyPowerControl();
            if (sd_card_ok) logToSDCard();
            updateOLED();
            Serial.printf("自动截止：充满 %dmV >= %dmV\n", batteryVoltage, bleChargeCutoff);
        }
    } else if (currentState == STATE_DISCHARGE_RUN) {
        if (batteryVoltage <= bleDischargeCutoff) {
            currentState = STATE_STOP;
            lastEvent = EVENT_AUTO_CUTOFF_EMPTY;
            updatePhase(0);  // 回到休息状态
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
            if (fromStop) {
                cycleNumber++;
                cycleMahIn = 0;
                cycleMahOut = 0;
            }
            break;
        case STATE_CHARGE_RUN:       currentState = STATE_CHARGE_PAUSE; break;
        case STATE_CHARGE_PAUSE:     currentState = STATE_CHARGE_RUN; break;
        case STATE_DISCHARGE_RUN:    currentState = STATE_DISCHARGE_PAUSE; break;
        case STATE_DISCHARGE_PAUSE:  currentState = STATE_DISCHARGE_RUN; break;
    }
    // 更新阶段类型
    if (currentState == STATE_CHARGE_RUN || currentState == STATE_CHARGE_PAUSE) {
        updatePhase(1);  // 充电
    } else if (currentState == STATE_DISCHARGE_RUN || currentState == STATE_DISCHARGE_PAUSE) {
        updatePhase(2);  // 放电
    } else {
        updatePhase(0);  // 休息
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
        updatePhase(0);  // 回到休息状态
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
        checkAutoCalibrate();  // 自动校准（在readBatteryData之前）
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
