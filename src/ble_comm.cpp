#include "ble_comm.h"
#include "main.h"
#include "display.h"
#include "sdcard.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>

// ================= BLE UUID =================
#define DEVICE_NAME             "ESP32_BMS_PRO"
#define CUSTOM_SERVICE_UUID     "0000ffe0-0000-1000-8000-00805f9b34fb"
#define TX_CHARACTERISTIC_UUID  "0000ffe1-0000-1000-8000-00805f9b34fb"
#define RX_CHARACTERISTIC_UUID  "0000ffe2-0000-1000-8000-00805f9b34fb"

// ================= BLE 对象 =================
static BLEServer *pServer = nullptr;
static BLECharacteristic *pTxChar = nullptr;
bool bleDeviceConnected = false;

// ================= 截止电压变量 =================
uint16_t bleChargeCutoff = 4200;
uint16_t bleDischargeCutoff = 3000;

// ================= 指令缓存（BLE回调只写，主loop读） =================
static volatile uint8_t  bleCmd = 0;
static volatile uint8_t  bleCmdData[2] = {0, 0};
static portMUX_TYPE bleMux = portMUX_INITIALIZER_UNLOCKED;

// ================= Status 字节编码 =================
static uint8_t encodeStatus() {
    uint8_t s = 0;
    if (currentMode == MODE_CHARGE) s |= (1 << 0);
    uint8_t gear = (currentMode == MODE_CHARGE) ? chargeGear : dischargeGear;
    if (gear == 2) s |= (1 << 1);
    uint8_t run = 0;
    switch (currentState) {
        case STATE_CHARGE_PAUSE:
        case STATE_DISCHARGE_PAUSE: run = 1; break;
        case STATE_CHARGE_RUN:
        case STATE_DISCHARGE_RUN:   run = 2; break;
        default: run = 0; break;
    }
    s |= (run & 0x03) << 2;
    return s;
}

// ================= 打包 24 字节上行帧 =================
static uint8_t packDataFrame(uint8_t *buf) {
    uint8_t idx = 0;
    buf[idx++] = 0xAA;
    buf[idx++] = 0x55;
    buf[idx++] = 0x10;

    buf[idx++] = encodeStatus();

    uint16_t v = (bq27220_ok && batteryVoltage >= 0) ? (uint16_t)batteryVoltage : 0;
    buf[idx++] = v >> 8;
    buf[idx++] = v & 0xFF;

    int16_t c = (bq27220_ok) ? (int16_t)batteryCurrent : 0;
    buf[idx++] = (c >> 8) & 0xFF;
    buf[idx++] = c & 0xFF;

    buf[idx++] = (bq27220_ok && !isnan(batteryTemp)) ? (uint8_t)batteryTemp : 0;

    buf[idx++] = (bq27220_ok && batterySOC >= 0) ? (uint8_t)batterySOC : 0;

    uint16_t cap = (bq27220_ok && batteryRemainCap >= 0) ? (uint16_t)batteryRemainCap : 0;
    buf[idx++] = cap >> 8;
    buf[idx++] = cap & 0xFF;

    buf[idx++] = (bq27220_ok && batterySOH >= 0) ? (uint8_t)batterySOH : 0;

    uint16_t cyc = (bq27220_ok && batteryCycleCount >= 0) ? (uint16_t)batteryCycleCount : 0;
    buf[idx++] = cyc >> 8;
    buf[idx++] = cyc & 0xFF;

    uint16_t tte = (bq27220_ok && batteryTTE >= 0) ? (uint16_t)batteryTTE : 0;
    buf[idx++] = tte >> 8;
    buf[idx++] = tte & 0xFF;

    uint16_t ttf = (bq27220_ok && batteryTTF >= 0) ? (uint16_t)batteryTTF : 0;
    buf[idx++] = ttf >> 8;
    buf[idx++] = ttf & 0xFF;

    buf[idx++] = bleChargeCutoff >> 8;
    buf[idx++] = bleChargeCutoff & 0xFF;
    buf[idx++] = bleDischargeCutoff >> 8;
    buf[idx++] = bleDischargeCutoff & 0xFF;

    uint8_t checksum = 0;
    for (uint8_t i = 2; i < idx; i++) checksum ^= buf[i];
    buf[idx++] = checksum;

    return idx;
}

// ================= BLE 回调类 =================
class SrvCallback : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer) {
        bleDeviceConnected = true;
        Serial.println("BLE: 客户端已连接");
    }
    void onDisconnect(BLEServer *pServer) {
        bleDeviceConnected = false;
        // 清空未处理的指令，防止重连后执行旧命令
        portENTER_CRITICAL(&bleMux);
        bleCmd = 0;
        bleCmdData[0] = 0;
        bleCmdData[1] = 0;
        portEXIT_CRITICAL(&bleMux);
        Serial.println("BLE: 客户端已断开");
        pServer->getAdvertising()->start();
    }
};

class RxCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string raw = pCharacteristic->getValue();
        if (raw.length() == 0) return;

        uint8_t cmd = (uint8_t)raw[0];
        bleCmd = cmd;
        bleCmdData[0] = (raw.length() >= 2) ? (uint8_t)raw[1] : 0;
        bleCmdData[1] = (raw.length() >= 3) ? (uint8_t)raw[2] : 0;

        // 0x01 读数据 — 立即回复
        if (cmd == 0x01) {
            Serial.println("收到指令: 读取数据");
            uint8_t buf[32];
            uint8_t len = packDataFrame(buf);
            if (pTxChar) {
                pTxChar->setValue(buf, len);
                pTxChar->notify();
            }
            bleCmd = 0;  // 0x01 已处理，不需要主循环再处理
        }
    }
};

// ================= BLE 初始化 =================
void initBLE() {
    BLEDevice::init(DEVICE_NAME);

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new SrvCallback());

    BLEService *pService = pServer->createService(CUSTOM_SERVICE_UUID);

    pTxChar = pService->createCharacteristic(
        TX_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_NOTIFY);
    pTxChar->addDescriptor(new BLE2902());

    BLECharacteristic *pRxChar = pService->createCharacteristic(
        RX_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    pRxChar->setCallbacks(new RxCallback());

    pService->start();

    BLEAdvertising *pAdv = pServer->getAdvertising();
    pAdv->addServiceUUID(CUSTOM_SERVICE_UUID);
    pAdv->setScanResponse(true);
    pAdv->setMinPreferred(0x06);
    pAdv->setMinPreferred(0x12);
    pAdv->start();

    Serial.println("BLE 服务已启动: " DEVICE_NAME);
}

// ================= 查询连接状态 =================
bool bleIsConnected() {
    return bleDeviceConnected;
}

// ================= 主循环轮询：处理手机下发指令 =================
void blePollCommand() {
    // 使用临界区保护多变量读取的原子性（修复BLE竞态条件）
    portENTER_CRITICAL(&bleMux);
    uint8_t cmd = bleCmd;
    uint8_t d0 = bleCmdData[0];
    uint8_t d1 = bleCmdData[1];
    bleCmd = 0;
    portEXIT_CRITICAL(&bleMux);

    if (cmd == 0) return;

    switch (cmd) {
    case 0x02:  // 充/放模式
        if (currentState == STATE_STOP) {
            currentMode = (d0 != 0) ? MODE_CHARGE : MODE_DISCHARGE;
            lastEvent = EVENT_MANUAL_MODE;
            Serial.print("BLE指令: 切换为");
            Serial.println(currentMode == MODE_CHARGE ? "充电模式" : "放电模式");
        }
        break;

    case 0x03:  // 档位
        if (currentMode == MODE_CHARGE) {
            chargeGear = (d0 == 2) ? 2 : 1;
        } else {
            dischargeGear = (d0 == 2) ? 2 : 1;
        }
        lastEvent = EVENT_MANUAL_GEAR;
        applyPowerControl();
        Serial.print("BLE指令: 切换为");
        Serial.print(d0);
        Serial.println("档");
        break;

    case 0x04:  // 运行状态
        {
            uint8_t st = d0 & 0x03;
            switch (st) {
            case 0:  // 停止
                if (currentState != STATE_STOP) {
                    currentState = STATE_STOP;
                    lastEvent = EVENT_MANUAL_STOP;
                }
                break;
            case 1:  // 暂停
                if (currentState == STATE_CHARGE_RUN)      currentState = STATE_CHARGE_PAUSE;
                else if (currentState == STATE_DISCHARGE_RUN) currentState = STATE_DISCHARGE_PAUSE;
                break;
            case 2:  // 运行
                if (currentState == STATE_STOP || currentState == STATE_CHARGE_PAUSE || currentState == STATE_DISCHARGE_PAUSE) {
                    currentState = (currentMode == MODE_CHARGE) ? STATE_CHARGE_RUN : STATE_DISCHARGE_RUN;
                    lastEvent = EVENT_MANUAL_START;
                }
                break;
            }
            applyPowerControl();
            if (sd_card_ok) logToSDCard();
            Serial.print("BLE指令: 运行状态 -> ");
            switch (st) {
            case 0: Serial.println("停止"); break;
            case 1: Serial.println("暂停"); break;
            case 2: Serial.println("运行"); break;
            }
        }
        break;

    case 0x05:  // 充电截止电压
        {
            uint16_t val = ((uint16_t)d0 << 8) | d1;
            if (val <= 4200 && val > bleDischargeCutoff) {
                bleChargeCutoff = val;
                Serial.print("BLE指令: 充电截止电压 = ");
                Serial.print(val / 1000.0f, 2);
                Serial.println("V");
            }
        }
        break;

    case 0x06:  // 放电截止电压
        {
            uint16_t val = ((uint16_t)d0 << 8) | d1;
            if (val >= 3000 && val < bleChargeCutoff) {
                bleDischargeCutoff = val;
                Serial.print("BLE指令: 放电截止电压 = ");
                Serial.print(val / 1000.0f, 2);
                Serial.println("V");
            }
        }
        break;

    default:
        break;
    }

    updateOLED();
}

// ================= 上报数据到手机 =================
void bleUpdateAndNotify() {
    if (!bleDeviceConnected || !pTxChar) return;

    uint8_t buf[32];
    uint8_t len = packDataFrame(buf);
    pTxChar->setValue(buf, len);
    pTxChar->notify();
}
