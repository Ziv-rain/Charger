# BQ27220 参数配置问题

## 目标

将BQ27220电量计的默认配置从3000mAh修改为1500mAh。

## 尝试的方法

### 1. 写入Data Memory RAM

地址：0x929F（Design Capacity）、0x929D（Full Charge Capacity）等。

- 写入操作显示成功
- 写入后立即验证显示1500mAh
- 退出Config Update模式后，Command 0x3C和FCC仍然显示3000mAh
- RM值超过1500mAh，说明SOC计算使用3000mAh默认值

### 2. 写入所有CEDV参数

参数：EMF、R0、R1、C0、C1、T0、TC、EDV阈值、电压-DOD曲线

- 结果相同：RAM写入成功，但不影响实际行为

### 3. 尝试的不同方法

- 使用库的`writeDataMemoryU16()`函数
- 使用原始I2C命令和正确的checksum计算（技术手册第65页的替换法）
- 验证已进入Config Update模式（OperationStatus bit2 = 1）

## 关键观察

1. **RAM写入成功** - 写入后立即回读显示正确值
2. **命令寄存器读取NVM** - 退出Config Update后0x3C（DesignCapacity）始终返回3000mAh
3. **SOC计算使用NVM** - RM = 2473mAh（3000mAh的82%），不是基于1500mAh
4. **根据技术手册第8.1节**："这些更新存储在RAM中，每次设备断电后都需要重新编程"
5. **即使写入所有参数**，行为也不会改变

## 问题

1. 仅通过I2C（不使用EV2300/EV2400）配置BQ27220不同电池容量的正确方法是什么？

2. 是否可以让RAM写入的参数对SOC计算生效？如果是，正确的序列是什么？

3. BQ27220是否支持通过I2C运行时修改参数？还是OTP编程是唯一的方法？

4. RAM写入Data Memory的用途是什么？如果它不影响电量计的行为？

5. 是否可以在没有BQStudio/EV2300的情况下生成gm.fs文件？或者有其他配置BQ27220的方法？

## 环境

- BQ27220YZFR
- ESP32（Arduino框架）
- I2C通信，100kHz
- 没有EV2300/EV2400

## 参考文档

- BQ27220数据表（SLUSCB7）
- BQ27220技术参考手册（SLUUBD4A）
