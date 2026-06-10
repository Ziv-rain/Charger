#pragma once

// ================= GB2312 编码的中文字符串（JC144 串口屏专用） =================
// JC144 内置 GB2312 中文字库，不支持 UTF-8，必须用 GB2312 编码

// 屏幕就绪
const char STR_READY[]       = "\xC6\xC1\xC4\xBB\xBE\xCD\xD0\xF7";
// 初始化GPIO
const char STR_INIT_GPIO[]   = "\xB3\xF5\xCA\xBC\xBB\xAF GPIO";
// 检测电池芯片
const char STR_DETECT_BQ[]   = "\xBC\xEC\xB2\xE2\xB5\xE7\xB3\xD8\xD0\xBE\xC6\xAC";
// 电池芯片就绪
const char STR_BQ_READY[]    = "\xB5\xE7\xB3\xD8\xD0\xBE\xC6\xAC\xBE\xCD\xD0\xF7";
// 芯片检测失败
const char STR_BQ_FAIL[]     = "\xD0\xBE\xC6\xAC\xBC\xEC\xB2\xE2\xCA\xA7\xB0\xDC";
// 检测存储卡
const char STR_DETECT_SD[]   = "\xBC\xEC\xB2\xE2\xB4\xE6\xB4\xA2\xBF\xA8";
// 存储卡就绪
const char STR_SD_READY[]    = "\xB4\xE6\xB4\xA2\xBF\xA8\xBE\xCD\xD0\xF7";
// 存储卡失败
const char STR_SD_FAIL[]     = "\xB4\xE6\xB4\xA2\xBF\xA8\xCA\xA7\xB0\xDC";
// 未检测到存储卡
const char STR_NO_SD[]       = "\xCE\xB4\xBC\xEC\xB2\xE2\xB5\xBD\xB4\xE6\xB4\xA2\xBF\xA8";
// 初始化蓝牙
const char STR_INIT_BLE[]    = "\xB3\xF5\xCA\xBC\xBB\xAF\xC0\xB6\xD1\xC0";
// 加载AI模型
const char STR_LOAD_AI[]     = "\xBC\xD3\xD4\xD8 AI\xC4\xA3\xD0\xCD";
// 初始化完成
const char STR_INIT_DONE[]   = "\xB3\xF5\xCA\xBC\xBB\xAF\xCD\xEA\xB3\xC9";
// 初始化中
const char STR_INIT_ING[]    = "\xB3\xF5\xCA\xBC\xBB\xAF\xD6\xD0";
// 电量（SOC 标签）
const char STR_LABEL_SOC[]   = "\xB5\xE7\xC1\xBF";
// 温度
const char STR_LABEL_TEMP[]  = "\xCE\xC2\xB6\xC8";
// 电压
const char STR_LABEL_VOLT[]  = "\xB5\xE7\xD1\xB9";
// 电流
const char STR_LABEL_CURR[]  = "\xB5\xE7\xC1\xF7";
// 充电 / 放电
const char STR_CHARGE[]      = "\xB3\xE4\xB5\xE7";
const char STR_DISCHARGE[]   = "\xB7\xC5\xB5\xE7";
// 档
const char STR_GEAR[]        = "\xB5\xB5";
// 运行 / 暂停 / 停止
const char STR_RUN[]         = "\xD4\xCB\xD0\xD0";
const char STR_PAUSE[]       = "\xD4\xDD\xCD\xA3";
const char STR_STOP[]        = "\xCD\xA3\xD6\xB9";
