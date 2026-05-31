#pragma once

#include <Arduino.h>

// ================= AI SOC预测模块 =================
// 使用TFLite Micro运行LSTM模型，预测电池SOC和剩余容量
// 输入：10个时间步 × 4特征（电压、电流、平均电流、温度）
// 输出：SOC% 和 剩余容量mAh

/**
 * 初始化AI推理模块
 * 加载TFLite模型，分配tensor arena
 * @return true=初始化成功，false=失败
 */
bool ai_soc_init();

/**
 * 更新特征缓冲区（无论是否在AI模式都应调用）
 * 将原始传感器数据归一化后存入环形缓冲区
 * @param voltage_mV  电压(mV)
 * @param current_mA  瞬时电流(mA)
 * @param avg_current_mA  平均电流(mA)
 * @param temperature_C  温度(°C)
 */
void ai_soc_update_buffer(int voltage_mV, int current_mA, int avg_current_mA, float temperature_C);

/**
 * 执行AI推理
 * 需要缓冲区满（至少10个样本）才能推理
 * @param soc_out  输出：预测SOC (0-100)
 * @param rm_out   输出：预测剩余容量 (mAh)
 * @return true=推理成功，false=缓冲区不足或推理失败
 */
bool ai_soc_predict(int* soc_out, int* rm_out);
