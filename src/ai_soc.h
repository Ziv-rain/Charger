#pragma once

// ================= AI SOC预测模块 =================
// 纯C实现LSTM推理，无TFLite依赖
// 输入：10个时间步 × 4特征（电压、电流、平均电流、温度）
// 输出：SOC% 和 剩余容量mAh

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化AI推理模块
 * @return true=初始化成功
 */
bool ai_soc_init(void);

/**
 * 更新特征缓冲区（无论是否在AI模式都应调用）
 * @param voltage_mV  电压(mV)
 * @param current_mA  瞬时电流(mA)
 * @param avg_current_mA  平均电流(mA)
 * @param temperature_C  温度(°C)
 */
void ai_soc_update_buffer(int voltage_mV, int current_mA, int avg_current_mA, float temperature_C);

/**
 * 执行AI推理
 * @param soc_out  输出：预测SOC (0-100)
 * @param rm_out   输出：预测剩余容量 (mAh)
 * @return true=推理成功
 */
bool ai_soc_predict(int *soc_out, int *rm_out);

#ifdef __cplusplus
}
#endif
