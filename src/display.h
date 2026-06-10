#pragma once
#include "main.h"

// ================= JC144 串口屏引脚定义 =================
#define UART_SCREEN_TX  17  // ESP32 TX -> JC144 Host_RX
#define UART_SCREEN_RX  16  // ESP32 RX -> JC144 Host_TX
#define UART_SCREEN_BAUD 115200

// ================= JC144 颜色定义 (4-bit, 0-15) =================
#define COLOR_BLACK       0
#define COLOR_RED         1
#define COLOR_GREEN       2
#define COLOR_YELLOW      3
#define COLOR_BLUE        4
#define COLOR_MAGENTA     5
#define COLOR_CYAN        6
#define COLOR_LIGHT_GRAY  7
#define COLOR_DARK_GRAY   8
#define COLOR_LIGHT_RED   9
#define COLOR_LIGHT_GREEN 10
#define COLOR_LIGHT_YELLOW 11
#define COLOR_LIGHT_BLUE  12
#define COLOR_LIGHT_MAGENTA 13
#define COLOR_LIGHT_CYAN  14
#define COLOR_WHITE       15

// ================= 函数声明 =================
void initScreen();
void screenClear(uint8_t color = COLOR_BLACK);
void screenSetBacklight(uint8_t level);  // 0=最亮, 255=最暗
void screenSetDirection(uint8_t dir);    // 0-3
void screenDrawText16(int x, int y, const char* text, uint8_t color);
void screenDrawText24(int x, int y, const char* text, uint8_t color);
void screenDrawText32(int x, int y, const char* text, uint8_t color);
void screenDrawLine(int x1, int y1, int x2, int y2, uint8_t color);
void screenDrawBox(int x1, int y1, int x2, int y2, uint8_t color);
void screenDrawBoxFilled(int x1, int y1, int x2, int y2, uint8_t color);
void screenDrawCircle(int x, int y, int r, uint8_t color);
void screenDrawCircleFilled(int x, int y, int r, uint8_t color);
void screenSetBgColor(uint8_t color);
void screenSendCommand(const char* cmd);
void screenWaitOk();
void drawProgressBar(int percent, const char* status);
void updateOLED();  // 保持原有函数名，内部实现改为串口屏
