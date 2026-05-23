#include "display.h"

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
