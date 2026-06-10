#include "display.h"
#include "ai_soc.h"

// ================= JC144 串口屏 UART 对象 =================
static HardwareSerial ScreenSerial(2);  // 使用 ESP32 Serial2
static bool layoutDrawn = false;  // 静态布局只画一次

// ================= 等待 OK 响应 =================
void screenWaitOk() {
    unsigned long start = millis();
    char buf[8];
    uint8_t idx = 0;
    while (millis() - start < 200) {
        if (ScreenSerial.available()) {
            char c = ScreenSerial.read();
            if (idx < sizeof(buf) - 1) {
                buf[idx++] = c;
                buf[idx] = '\0';
            }
            if (strstr(buf, "OK") != nullptr) {
                return;
            }
        }
    }
}

// ================= 发送指令 =================
void screenSendCommand(const char* cmd) {
    ScreenSerial.print(cmd);
    ScreenSerial.print(";\r\n");
    screenWaitOk();
}

// ================= 初始化串口屏 =================
void initScreen() {
    ScreenSerial.begin(UART_SCREEN_BAUD, SERIAL_8N1, UART_SCREEN_RX, UART_SCREEN_TX);
    delay(1000);

    // 开背光 + 清屏
    ScreenSerial.print("BL(0);\r\n");
    delay(30);
    ScreenSerial.print("DIR(0);\r\n");
    delay(30);
    ScreenSerial.print("CLR(0);\r\n");

    layoutDrawn = false;
    Serial.println("JC144 串口屏初始化完成");
}

// ================= 清屏 =================
void screenClear(uint8_t color) {
    char buf[32];
    snprintf(buf, sizeof(buf), "CLR(%d)", color);
    screenSendCommand(buf);
    layoutDrawn = false;
}

// ================= 设置背光 =================
void screenSetBacklight(uint8_t level) {
    char buf[32];
    snprintf(buf, sizeof(buf), "BL(%d)", level);
    screenSendCommand(buf);
}

// ================= 设置显示方向 =================
void screenSetDirection(uint8_t dir) {
    char buf[32];
    snprintf(buf, sizeof(buf), "DIR(%d)", dir);
    screenSendCommand(buf);
}

// ================= 设置背景色 =================
void screenSetBgColor(uint8_t color) {
    char buf[32];
    snprintf(buf, sizeof(buf), "SBC(%d)", color);
    screenSendCommand(buf);
}

// ================= 绘制 16x16 文字 =================
void screenDrawText16(int x, int y, const char* text, uint8_t color) {
    char buf[128];
    snprintf(buf, sizeof(buf), "DC16(%d,%d,'%s',%d)", x, y, text, color);
    screenSendCommand(buf);
}

// ================= 绘制 24x24 文字 =================
void screenDrawText24(int x, int y, const char* text, uint8_t color) {
    char buf[128];
    snprintf(buf, sizeof(buf), "DC24(%d,%d,'%s',%d)", x, y, text, color);
    screenSendCommand(buf);
}

// ================= 绘制 32x32 文字 =================
void screenDrawText32(int x, int y, const char* text, uint8_t color) {
    char buf[128];
    snprintf(buf, sizeof(buf), "DC32(%d,%d,'%s',%d)", x, y, text, color);
    screenSendCommand(buf);
}

// ================= 绘制直线 =================
void screenDrawLine(int x1, int y1, int x2, int y2, uint8_t color) {
    char buf[64];
    snprintf(buf, sizeof(buf), "PL(%d,%d,%d,%d,%d)", x1, y1, x2, y2, color);
    screenSendCommand(buf);
}

// ================= 绘制矩形框 =================
void screenDrawBox(int x1, int y1, int x2, int y2, uint8_t color) {
    char buf[64];
    snprintf(buf, sizeof(buf), "BOX(%d,%d,%d,%d,%d)", x1, y1, x2, y2, color);
    screenSendCommand(buf);
}

// ================= 绘制填充矩形 =================
void screenDrawBoxFilled(int x1, int y1, int x2, int y2, uint8_t color) {
    char buf[64];
    snprintf(buf, sizeof(buf), "BOXF(%d,%d,%d,%d,%d)", x1, y1, x2, y2, color);
    screenSendCommand(buf);
}

// ================= 绘制圆 =================
void screenDrawCircle(int x, int y, int r, uint8_t color) {
    char buf[64];
    snprintf(buf, sizeof(buf), "CIR(%d,%d,%d,%d)", x, y, r, color);
    screenSendCommand(buf);
}

// ================= 绘制填充圆 =================
void screenDrawCircleFilled(int x, int y, int r, uint8_t color) {
    char buf[64];
    snprintf(buf, sizeof(buf), "CIRF(%d,%d,%d,%d)", x, y, r, color);
    screenSendCommand(buf);
}

// ================= 进度条绘制 =================
void drawProgressBar(int percent, const char* status) {
    screenClear(COLOR_BLACK);
    screenDrawText16(20, 10, "INITIALIZING", COLOR_WHITE);
    screenDrawBox(10, 30, 117, 42, COLOR_DARK_GRAY);
    int fillWidth = (percent * 104) / 100;
    if (fillWidth > 0) {
        screenDrawBoxFilled(12, 32, 12 + fillWidth, 40, COLOR_GREEN);
    }
    char percentStr[16];
    snprintf(percentStr, sizeof(percentStr), "%d%%", percent);
    screenDrawText16(48, 50, percentStr, COLOR_WHITE);
    screenDrawText16(10, 70, status, COLOR_LIGHT_GRAY);
}

// ================= 擦除指定区域（填充黑色方块） =================
static void eraseArea(int x, int y, int w, int h) {
    screenDrawBoxFilled(x, y, x + w - 1, y + h - 1, COLOR_BLACK);
}

// ================= 绘制静态布局（只执行一次） =================
static void drawStaticLayout() {
    screenClear(COLOR_BLACK);

    // 状态栏分隔线
    screenDrawLine(0, 18, 127, 18, COLOR_DARK_GRAY);
    // 模式分隔线
    screenDrawLine(0, 40, 127, 40, COLOR_DARK_GRAY);
    // 底部数据分隔线
    screenDrawLine(0, 86, 127, 86, COLOR_DARK_GRAY);

    // 标签 - 左侧温度、右侧数据标签
    screenDrawText16(0, 88, "TEMP", COLOR_DARK_GRAY);
    screenDrawText16(64, 88, "VOLT", COLOR_DARK_GRAY);
    screenDrawText16(64, 108, "CURR", COLOR_DARK_GRAY);

    layoutDrawn = true;
}

// ================= 更新显示（无闪烁局部刷新） =================
void updateOLED() {
    if (!layoutDrawn) {
        drawStaticLayout();
    }

    // === 状态栏 y=0~18 ===

    // SD 状态
    eraseArea(0, 0, 24, 16);
    screenDrawText16(0, 0, sd_card_ok ? "SD" : "--", sd_card_ok ? COLOR_GREEN : COLOR_DARK_GRAY);

    // BLE 状态
    eraseArea(26, 0, 24, 16);
    screenDrawText16(26, 0, bleDeviceConnected ? "BLE" : "--", bleDeviceConnected ? COLOR_CYAN : COLOR_DARK_GRAY);

    // AI 模式指示
    eraseArea(52, 0, 22, 16);
    if (aiMode) {
        screenDrawBoxFilled(52, 0, 74, 16, COLOR_YELLOW);
        screenDrawText16(54, 0, "AI", COLOR_BLACK);
    }

    // 剩余容量 / 事件
    eraseArea(78, 0, 50, 16);
    if (lastEvent == EVENT_AUTO_CUTOFF_FULL) {
        screenDrawText16(78, 0, "FULL", COLOR_GREEN);
    } else if (lastEvent == EVENT_AUTO_CUTOFF_EMPTY) {
        screenDrawText16(78, 0, "LOW!", COLOR_RED);
    } else if (bq27220_ok && batteryRemainCap >= 0) {
        char capStr[16];
        snprintf(capStr, sizeof(capStr), "%dmAh", batteryRemainCap);
        screenDrawText16(78, 0, capStr, COLOR_WHITE);
    } else {
        screenDrawText16(78, 0, "---mAh", COLOR_DARK_GRAY);
    }

    // === 第2行 y=22~38：模式 档位 状态 ===
    eraseArea(0, 22, 127, 16);

    const char* modeStr = (currentMode == MODE_CHARGE) ? "CHG" : "DIS";
    int gear = (currentMode == MODE_CHARGE) ? chargeGear : dischargeGear;
    const char* stateStr = "STOP";
    switch (currentState) {
        case STATE_STOP:             stateStr = "STOP"; break;
        case STATE_CHARGE_RUN:
        case STATE_DISCHARGE_RUN:    stateStr = "RUN";  break;
        case STATE_CHARGE_PAUSE:
        case STATE_DISCHARGE_PAUSE:  stateStr = "PAUSE"; break;
    }

    char line2[32];
    snprintf(line2, sizeof(line2), "%s  G%d  %s", modeStr, gear, stateStr);
    screenDrawText16(0, 22, line2, COLOR_WHITE);

    // === 第3行 y=44~84：SOC 大字 ===
    eraseArea(0, 44, 127, 40);
    if (bq27220_ok && batterySOC >= 0) {
        char socStr[16];
        snprintf(socStr, sizeof(socStr), "SOC %d%%", batterySOC);
        screenDrawText24(10, 48, socStr, COLOR_GREEN);
    } else {
        screenDrawText24(10, 48, "SOC  --%", COLOR_DARK_GRAY);
    }

    // === 底部数据区 y=90~126 ===

    // 温度（左侧）
    eraseArea(0, 100, 60, 16);
    if (bq27220_ok && !isnan(batteryTemp)) {
        char tempStr[16];
        snprintf(tempStr, sizeof(tempStr), "%.1fC", batteryTemp);
        screenDrawText16(0, 100, tempStr, COLOR_CYAN);
    } else {
        screenDrawText16(0, 100, "--.-C", COLOR_DARK_GRAY);
    }

    // 电压（右上）
    eraseArea(64, 100, 64, 16);
    if (bq27220_ok && batteryVoltage >= 0) {
        char voltStr[16];
        snprintf(voltStr, sizeof(voltStr), "%.2fV", batteryVoltage / 1000.0);
        screenDrawText16(64, 100, voltStr, COLOR_YELLOW);
    } else {
        screenDrawText16(64, 100, "--.--V", COLOR_DARK_GRAY);
    }

    // 电流（右下）
    eraseArea(64, 116, 64, 16);
    if (bq27220_ok) {
        char currStr[16];
        snprintf(currStr, sizeof(currStr), "%+dmA", batteryCurrent);
        screenDrawText16(64, 116, currStr, COLOR_LIGHT_BLUE);
    } else {
        screenDrawText16(64, 116, "---mA", COLOR_DARK_GRAY);
    }
}
