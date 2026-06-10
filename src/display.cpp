#include "display.h"
#include "display_strings.h"
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

// ================= 擦除指定区域（填充黑色方块） =================
static void eraseArea(int x, int y, int w, int h) {
    screenDrawBoxFilled(x, y, x + w - 1, y + h - 1, COLOR_BLACK);
}

// ================= 进度条绘制 =================
void drawProgressBar(int percent, const char* status) {
    // 首次调用时画固定内容
    static bool frameDrawn = false;
    if (!frameDrawn) {
        screenClear(COLOR_BLACK);
        frameDrawn = true;
    }

    // 顶部标题（16px 字高完整覆盖 y=15~31）
    eraseArea(0, 5, 128, 30);
    screenDrawText16(28, 15, STR_INIT_ING, COLOR_WHITE);

    // 分隔线（从 y=36 开始擦，避免切到文字）
    eraseArea(0, 36, 128, 5);
    screenDrawLine(10, 38, 117, 38, COLOR_LIGHT_GRAY);

    // 进度条背景
    screenDrawBoxFilled(10, 50, 117, 64, COLOR_DARK_GRAY);

    // 进度条填充
    int barWidth = (percent * 104) / 100;
    if (barWidth > 0) {
        screenDrawBoxFilled(12, 52, 12 + barWidth, 62, COLOR_GREEN);
    }

    // 进度条外框
    screenDrawBox(10, 50, 117, 64, COLOR_LIGHT_GRAY);

    // 百分比
    char percentStr[16];
    snprintf(percentStr, sizeof(percentStr), "%d%%", percent);
    eraseArea(0, 72, 128, 16);
    screenDrawText16(52, 72, percentStr, COLOR_WHITE);

    // 状态文字（靠近底边）
    eraseArea(0, 102, 128, 18);
    screenDrawText16(16, 104, status, COLOR_LIGHT_GRAY);
}

// ================= 更新显示（完整 UI） =================
void updateOLED() {
    // 首次调用清屏（清除初始页面的残留元素）
    static bool firstFrame = true;
    if (firstFrame) {
        screenClear(COLOR_BLACK);
        firstFrame = false;
    }

    // === 第1行 y=0~16：SD + BLE + AI + 容量（紧凑排列） ===
    eraseArea(0, 0, 18, 16);
    screenDrawText16(0, 0, sd_card_ok ? "SD" : "--", sd_card_ok ? COLOR_GREEN : COLOR_DARK_GRAY);

    eraseArea(20, 0, 24, 16);
    screenDrawText16(20, 0, bleDeviceConnected ? "BLE" : "--", bleDeviceConnected ? COLOR_BLUE : COLOR_DARK_GRAY);

    // AI 模式指示（黄色框居中）
    eraseArea(46, 0, 24, 18);
    if (aiMode) {
        screenDrawBoxFilled(46, 0, 68, 16, COLOR_YELLOW);
        screenDrawText16(51, 0, "AI", COLOR_BLACK);  // 居中于 46~68
    }

    // 剩余容量（预留右侧足够空间）
    eraseArea(72, 0, 56, 16);
    if (lastEvent == EVENT_AUTO_CUTOFF_FULL) {
        screenDrawText16(72, 0, "FULL", COLOR_GREEN);
    } else if (lastEvent == EVENT_AUTO_CUTOFF_EMPTY) {
        screenDrawText16(72, 0, "LOW", COLOR_RED);
    } else if (bq27220_ok && batteryRemainCap >= 0) {
        char capStr[16];
        snprintf(capStr, sizeof(capStr), "%dmAh", batteryRemainCap);
        screenDrawText16(72, 0, capStr, COLOR_WHITE);
    } else {
        screenDrawText16(72, 0, "---mAh", COLOR_DARK_GRAY);
    }

    // 分隔线
    screenDrawLine(0, 18, 127, 18, COLOR_LIGHT_GRAY);

    // === 第2行 y=22~36：模式左对齐  档位居中  状态右对齐 ===
    eraseArea(0, 22, 128, 16);
    const char* modeStr = (currentMode == MODE_CHARGE) ? STR_CHARGE : STR_DISCHARGE;
    int gear = (currentMode == MODE_CHARGE) ? chargeGear : dischargeGear;
    const char* stateStr;
    switch (currentState) {
        case STATE_STOP:             stateStr = STR_STOP; break;
        case STATE_CHARGE_RUN:
        case STATE_DISCHARGE_RUN:    stateStr = STR_RUN;  break;
        case STATE_CHARGE_PAUSE:
        case STATE_DISCHARGE_PAUSE:  stateStr = STR_PAUSE; break;
        default:                     stateStr = STR_STOP; break;
    }
    // 模式左对齐
    screenDrawText16(0, 22, modeStr, COLOR_WHITE);
    // 档位居中
    char gearStr[8];
    snprintf(gearStr, sizeof(gearStr), "%d%s", gear, STR_GEAR);
    screenDrawText16(52, 22, gearStr, COLOR_WHITE);
    // 状态右对齐
    screenDrawText16(96, 22, stateStr, COLOR_WHITE);

    // 分隔线
    screenDrawLine(0, 38, 127, 38, COLOR_LIGHT_GRAY);

    // === 2x2 田字格（y=40~127） ===
    screenDrawLine(64, 40, 64, 127, COLOR_LIGHT_GRAY);
    screenDrawLine(0, 83, 127, 83, COLOR_LIGHT_GRAY);

    // 左上：SOC（居中于 0~63）
    screenDrawText16(20, 46, "SOC", COLOR_WHITE);
    eraseArea(4, 64, 56, 16);
    if (batterySOC >= 0) {
        char val[16];
        int w = snprintf(val, sizeof(val), "%d%%", batterySOC);
        screenDrawText16(32 - w * 4, 64, val, COLOR_WHITE);
    } else {
        screenDrawText16(20, 64, "--%", COLOR_DARK_GRAY);
    }

    // 右上：温度（居中于 64~127）
    screenDrawText16(80, 46, STR_LABEL_TEMP, COLOR_WHITE);
    eraseArea(68, 64, 58, 16);
    if (bq27220_ok && !isnan(batteryTemp)) {
        char val[16];
        int w = snprintf(val, sizeof(val), "%.1fC", batteryTemp);
        screenDrawText16(96 - w * 4, 64, val, COLOR_WHITE);
    } else {
        screenDrawText16(76, 64, "--.-C", COLOR_DARK_GRAY);
    }

    // 左下：电压（居中于 0~63）
    screenDrawText16(16, 88, STR_LABEL_VOLT, COLOR_WHITE);
    eraseArea(4, 106, 56, 16);
    if (batteryVoltage >= 0) {
        char val[16];
        int w = snprintf(val, sizeof(val), "%.2fV", batteryVoltage / 1000.0);
        screenDrawText16(32 - w * 4, 106, val, COLOR_WHITE);
    } else {
        screenDrawText16(12, 106, "--.--V", COLOR_DARK_GRAY);
    }

    // 右下：电流（居中于 64~127）
    screenDrawText16(80, 88, STR_LABEL_CURR, COLOR_WHITE);
    eraseArea(68, 106, 60, 16);
    {
        char val[16];
        int w;
        if (batteryCurrent == 0) {
            strcpy(val, "0mA");
            w = 3;
        } else {
            w = snprintf(val, sizeof(val), "%+dmA", batteryCurrent);
        }
        screenDrawText16(96 - w * 4, 106, val, COLOR_WHITE);
    }
}
