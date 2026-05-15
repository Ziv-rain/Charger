#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <OneButton.h>

// ================= 引脚定义 =================
const int PIN_KEY1 = 34;  // 按键1引脚 (IO34，硬件已加上拉电阻和电容消抖)
const int PIN_KEY2 = 35;  // 按键2引脚 (IO35，硬件已加上拉电阻和电容消抖)
// OLED I2C引脚: SDA = 21, SCL = 22 (ESP32默认，U8g2会自动使用)

// ================= 初始化对象 =================
// 初始化 OLED (0.96寸 SSD1306, I2C硬件接口)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// 初始化按键 (引脚, 开启内部上拉电阻设为false)
OneButton button1(PIN_KEY1, true,false);
OneButton button2(PIN_KEY2, true,false);

// ================= 状态机枚举定义 =================
enum SystemState {
    STATE_STOP,             // 停止态
    STATE_CHARGE_RUN,       // 充电运行态
    STATE_CHARGE_PAUSE,     // 充电暂停态
    STATE_DISCHARGE_RUN,    // 放电运行态
    STATE_DISCHARGE_PAUSE   // 放电暂停态
};

enum WorkMode {
    MODE_CHARGE,            // 充电模式
    MODE_DISCHARGE          // 放电模式
};

// ================= 全局变量 =================
SystemState currentState = STATE_STOP;
WorkMode currentMode = MODE_CHARGE;
int chargeGear = 1;         // 充电档位: 1 或 2
int dischargeGear = 1;      // 放电档位: 1 或 2

// ================= OLED 刷新函数 (保留原样) =================
void updateOLED() {
    u8g2.clearBuffer(); // 清除内部缓冲区
    
    // 设置支持中文的字体 (文泉驿12号字)
    u8g2.setFont(u8g2_font_wqy12_t_gb2312); 
    u8g2.setCursor(0, 15);

    String displayStr = "";

    // 根据当前状态拼接显示字符串
    switch (currentState) {
        case STATE_STOP:
            displayStr += "停止 | ";
            displayStr += (currentMode == MODE_CHARGE) ? "充电模式 | " : "放电模式 | ";
            displayStr += String((currentMode == MODE_CHARGE) ? chargeGear : dischargeGear) + "档";
            break;
            
        case STATE_CHARGE_RUN:
            displayStr += "充电运行 | " + String(chargeGear) + "档";
            break;
            
        case STATE_CHARGE_PAUSE:
            displayStr += "充电暂停 | " + String(chargeGear) + "档";
            break;
            
        case STATE_DISCHARGE_RUN:
            displayStr += "放电运行 | " + String(dischargeGear) + "档";
            break;
            
        case STATE_DISCHARGE_PAUSE:
            displayStr += "放电暂停 | " + String(dischargeGear) + "档";
            break;
    }

    // 将字符串打印到 OLED
    u8g2.print(displayStr.c_str());

    // 预留区域显示其他信息
    u8g2.setCursor(0, 35);
    u8g2.print("SOC: --%");
    u8g2.setCursor(0, 55);
    u8g2.print("V: --.- V   I: --- mA");

    u8g2.sendBuffer(); // 发送到屏幕显示
    
    // 串口同步打印
    Serial.println(displayStr);
}

// ================= 按键事件回调函数 (已优化) =================

// Key1 短按：启动 / 暂停 / 继续
void clickKey1() {
    Serial.println("Key1 短按触发");
    switch (currentState) {
        case STATE_STOP:
            // 启动运行
            currentState = (currentMode == MODE_CHARGE) ? STATE_CHARGE_RUN : STATE_DISCHARGE_RUN;
            break;
        case STATE_CHARGE_RUN:
            // 暂停充电
            currentState = STATE_CHARGE_PAUSE;
            break;
        case STATE_CHARGE_PAUSE:
            // 继续充电
            currentState = STATE_CHARGE_RUN;
            break;
        case STATE_DISCHARGE_RUN:
            // 暂停放电
            currentState = STATE_DISCHARGE_PAUSE;
            break;
        case STATE_DISCHARGE_PAUSE:
            // 继续放电
            currentState = STATE_DISCHARGE_RUN;
            break;
    }
    updateOLED();
}

// Key1 长按释放：紧急停止，回到停止态
void longPressKey1() {
    Serial.println("Key1 长按释放触发 -> 紧急停止");
    if (currentState != STATE_STOP) {
        currentState = STATE_STOP;
        updateOLED();
    }
}

// Key2 短按：永远负责切换档位 (1档 <-> 2档)
void clickKey2() {
    Serial.println("Key2 短按触发 -> 切换档位");
    if (currentMode == MODE_CHARGE) {
        chargeGear = (chargeGear == 1) ? 2 : 1;
    } else {
        dischargeGear = (dischargeGear == 1) ? 2 : 1;
    }
    updateOLED();
}

// Key2 长按释放：仅在停止态切换充/放模式
void longPressKey2() {
    Serial.println("Key2 长按释放触发");
    if (currentState == STATE_STOP) {
        currentMode = (currentMode == MODE_CHARGE) ? MODE_DISCHARGE : MODE_CHARGE;
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
    
    // 初始化 OLED
    Wire.begin(21, 22); // 强制指定 I2C 引脚为 21(SDA), 22(SCL)
    u8g2.begin();
    u8g2.enableUTF8Print(); // 允许打印UTF8中文字符

    // 配置按键长按触发时间 (2000ms = 2秒)
    button1.setPressMs(1500); 
    button2.setPressMs(1500);

    // 绑定按键回调函数 (关键修改：改用 attachLongPressStop)
    button1.attachClick(clickKey1);
    button1.attachLongPressStop(longPressKey1); 
    
    button2.attachClick(clickKey2);
    button2.attachLongPressStop(longPressKey2);

    // 开机刷新一次初始界面
    updateOLED();
    Serial.println("初始化完成！");
}

void loop() {
    // 必须在循环中不断调用 tick()，库会自动处理消抖和长短按识别
    button1.tick();
    button2.tick();
}