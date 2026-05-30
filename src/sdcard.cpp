#include "sdcard.h"
#include <SD.h>
#include <SPI.h>

bool initSDCard() {
    SPI.begin(PIN_SD_CLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    SPI.setFrequency(25000000);
    delay(10);

    if (!SD.begin(PIN_SD_CS)) {
        Serial.println("SD卡初始化失败！");
        return false;
    }

    int n = 1;
    while (n <= 999) {
        snprintf(logFilename, sizeof(logFilename), "/charger_%03d.csv", n);
        if (!SD.exists(logFilename)) break;
        n++;
    }
    if (n > 999) {
        snprintf(logFilename, sizeof(logFilename), "/charger_%lu.csv", millis());
    }

    File logFile = SD.open(logFilename, FILE_WRITE);
    if (logFile) {
        logFile.println("timestamp_ms,state_id,mode_id,gear_id,"
                        "SOC_pct,custom_SOC_pct,custom_RM_mAh,"
                        "voltage_mV,current_mA,avg_current_mA,"
                        "temperature_C,remain_cap_mAh,full_cap_mAh,design_cap_mAh,"
                        "cycle_count_n,SOH_pct,tte_min,ttf_min,"
                        "batt_status_hex,op_status_hex,event_id,"
                        "cycle_number,phase_type,elapsed_phase_sec,"
                        "cumulative_mah_in,cumulative_mah_out,"
                        "cycle_mah_in,cycle_mah_out,"
                        "cumulative_wh,dv_dt_mV_per_s,estimated_IR_mOhm");
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
        sdFailCount++;
        if (sdFailCount > 3) {
            sd_card_ok = false;
            sdFailCount = 0;
            Serial.println("SD卡连续写入失败，暂停记录！");
        }
        return;
    }
    sdFailCount = 0;

    logFile.print(millis());
    logFile.print(",");
    logFile.print(currentState);
    logFile.print(",");
    logFile.print(currentMode);
    logFile.print(",");
    logFile.print((currentMode == MODE_CHARGE) ? chargeGear : dischargeGear);
    logFile.print(",");
    if (bq27220_ok) {
        logFile.print(batterySOC);
        logFile.print(",");
        logFile.print(customSOC);
        logFile.print(",");
        logFile.print(remainingCapacityMah, 1);
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
        logFile.print(lastEvent);
        // 新增字段
        logFile.print(",");
        logFile.print(cycleNumber);
        logFile.print(",");
        logFile.print(phaseType);
        logFile.print(",");
        logFile.print((millis() - phaseStartTime) / 1000);
        logFile.print(",");
        logFile.print(cumulativeMahIn, 1);
        logFile.print(",");
        logFile.print(cumulativeMahOut, 1);
        logFile.print(",");
        logFile.print(cycleMahIn, 1);
        logFile.print(",");
        logFile.print(cycleMahOut, 1);
        logFile.print(",");
        logFile.print(cumulativeWh, 2);
        logFile.print(",");
        logFile.print(dvDt, 2);
        logFile.print(",");
        logFile.println(estimatedIR, 1);
    } else {
        logFile.print(millis());
        logFile.print(",");
        logFile.print(currentState);
        logFile.print(",");
        logFile.print(currentMode);
        logFile.print(",");
        logFile.print((currentMode == MODE_CHARGE) ? chargeGear : dischargeGear);
        logFile.print(",-1,-1,-1,-1,-1,-1,NAN,-1,-1,-1,-1,-1,-1,-1,0x0000,0x0000,");
        logFile.print(lastEvent);
        logFile.print(",");
        logFile.print(cycleNumber);
        logFile.print(",");
        logFile.print(phaseType);
        logFile.print(",");
        logFile.print((millis() - phaseStartTime) / 1000);
        logFile.println(",-1,-1,-1,-1,-1,-1,-1");
    }
    lastEvent = EVENT_NONE;
    logFile.flush();  // 显式刷新，防止断电数据丢失
    logFile.close();
}
