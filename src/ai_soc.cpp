/**
 * 纯C LSTM推理引擎
 * 替代TFLite Micro，零框架依赖
 *
 * 模型结构:
 *   LSTM(32) -> LSTM(16) -> Dense(8,relu) -> Dense(2,linear)
 *   输入: [10, 4] (look_back=10, features=voltage/current/avg_current/temp)
 *   输出: [2] (SOC%, remaining_mAh)
 */

#include "ai_soc.h"
#include "lstm_weights.h"
#include <math.h>
#include <string.h>

// ================= 辅助函数 =================

static inline float sigmoidf(float x) {
    return 1.0f / (1.0f + expf(-x));
}

/**
 * 矩阵-向量乘法: out = W * x + b
 * W: [rows x cols] (行优先), x: [cols], b: [rows], out: [rows]
 */
static void matvec_add(const float *W, const float *x, const float *b,
                       float *out, int rows, int cols) {
    for (int i = 0; i < rows; i++) {
        float sum = b[i];
        const float *Wrow = W + i * cols;
        for (int j = 0; j < cols; j++) {
            sum += Wrow[j] * x[j];
        }
        out[i] = sum;
    }
}

/**
 * LSTM Cell单步计算
 */
static void lstm_cell(const float *x, const float *h_prev, const float *c_prev,
                      float *h_out, float *c_out,
                      const float *Wi, const float *Wf, const float *Wc, const float *Wo,
                      const float *Ui, const float *Uf, const float *Uc, const float *Uo,
                      const float *bi, const float *bf, const float *bc, const float *bo,
                      int input_dim, int units,
                      float *buf /* 至少 4*units */) {
    float *gi = buf;
    float *gf = buf + units;
    float *gc = buf + 2 * units;
    float *go = buf + 3 * units;

    // gi = sigmoid(Wi*x + Ui*h + bi)
    matvec_add(Wi, x, bi, gi, units, input_dim);
    for (int i = 0; i < units; i++) {
        float sum = 0;
        for (int j = 0; j < units; j++) sum += Ui[i * units + j] * h_prev[j];
        gi[i] = sigmoidf(gi[i] + sum);
    }

    // gf = sigmoid(Wf*x + Uf*h + bf)
    matvec_add(Wf, x, bf, gf, units, input_dim);
    for (int i = 0; i < units; i++) {
        float sum = 0;
        for (int j = 0; j < units; j++) sum += Uf[i * units + j] * h_prev[j];
        gf[i] = sigmoidf(gf[i] + sum);
    }

    // gc = tanh(Wc*x + Uc*h + bc)
    matvec_add(Wc, x, bc, gc, units, input_dim);
    for (int i = 0; i < units; i++) {
        float sum = 0;
        for (int j = 0; j < units; j++) sum += Uc[i * units + j] * h_prev[j];
        gc[i] = tanhf(gc[i] + sum);
    }

    // go = sigmoid(Wo*x + Uo*h + bo)
    matvec_add(Wo, x, bo, go, units, input_dim);
    for (int i = 0; i < units; i++) {
        float sum = 0;
        for (int j = 0; j < units; j++) sum += Uo[i * units + j] * h_prev[j];
        go[i] = sigmoidf(go[i] + sum);
    }

    // c = gf * c_prev + gi * gc
    // h = go * tanh(c)
    for (int i = 0; i < units; i++) {
        c_out[i] = gf[i] * c_prev[i] + gi[i] * gc[i];
        h_out[i] = go[i] * tanhf(c_out[i]);
    }
}

/**
 * Dense层: out = W*x + b, 可选ReLU
 */
static void dense_forward(const float *W, const float *x, const float *b,
                          float *out, int in_dim, int out_dim, bool use_relu) {
    for (int i = 0; i < out_dim; i++) {
        float sum = b[i];
        const float *Wrow = W + i * in_dim;
        for (int j = 0; j < in_dim; j++) {
            sum += Wrow[j] * x[j];
        }
        out[i] = use_relu ? fmaxf(0.0f, sum) : sum;
    }
}

// ================= 全局状态 =================

static float feature_buffer[LOOK_BACK][N_FEATURES];
static int buffer_index = 0;
static bool buffer_full = false;

// ================= 公共接口 =================

bool ai_soc_init(void) {
    buffer_index = 0;
    buffer_full = false;
    memset(feature_buffer, 0, sizeof(feature_buffer));
    return true;
}

void ai_soc_update_buffer(int voltage_mV, int current_mA, int avg_current_mA, float temperature_C) {
    float x[N_FEATURES];
    x[0] = ((float)voltage_mV - FEAT_MIN[0]) / FEAT_RANGE[0];
    x[1] = ((float)current_mA - FEAT_MIN[1]) / FEAT_RANGE[1];
    x[2] = ((float)avg_current_mA - FEAT_MIN[2]) / FEAT_RANGE[2];
    x[3] = (temperature_C - FEAT_MIN[3]) / FEAT_RANGE[3];

    for (int i = 0; i < N_FEATURES; i++) {
        if (x[i] < 0.0f) x[i] = 0.0f;
        if (x[i] > 1.0f) x[i] = 1.0f;
    }

    memcpy(feature_buffer[buffer_index], x, sizeof(float) * N_FEATURES);
    buffer_index = (buffer_index + 1) % LOOK_BACK;
    if (buffer_index == 0) buffer_full = true;
}

bool ai_soc_predict(int *out_soc, int *out_rm) {
    if (!buffer_full) return false;

    // LSTM层1状态
    static float h1[LSTM1_UNITS];
    static float c1[LSTM1_UNITS];
    memset(h1, 0, sizeof(h1));
    memset(c1, 0, sizeof(c1));

    // LSTM层2状态
    static float h2[LSTM2_UNITS];
    static float c2[LSTM2_UNITS];
    memset(h2, 0, sizeof(h2));
    memset(c2, 0, sizeof(c2));

    // 通用缓冲区 (4*max_units = 4*32 = 128)
    float buf[4 * LSTM1_UNITS];

    // 逐时间步处理
    for (int t = 0; t < LOOK_BACK; t++) {
        int idx = (buffer_index + t) % LOOK_BACK;
        const float *x = feature_buffer[idx];

        // LSTM层1
        float h1_new[LSTM1_UNITS], c1_new[LSTM1_UNITS];
        lstm_cell(x, h1, c1, h1_new, c1_new,
                  lstm1_Wi, lstm1_Wf, lstm1_Wc, lstm1_Wo,
                  lstm1_Ui, lstm1_Uf, lstm1_Uc, lstm1_Uo,
                  lstm1_bi, lstm1_bf, lstm1_bc, lstm1_bo,
                  N_FEATURES, LSTM1_UNITS, buf);
        memcpy(h1, h1_new, sizeof(h1));
        memcpy(c1, c1_new, sizeof(c1));

        // LSTM层2
        float h2_new[LSTM2_UNITS], c2_new[LSTM2_UNITS];
        lstm_cell(h1, h2, c2, h2_new, c2_new,
                  lstm2_Wi, lstm2_Wf, lstm2_Wc, lstm2_Wo,
                  lstm2_Ui, lstm2_Uf, lstm2_Uc, lstm2_Uo,
                  lstm2_bi, lstm2_bf, lstm2_bc, lstm2_bo,
                  LSTM1_UNITS, LSTM2_UNITS, buf);
        memcpy(h2, h2_new, sizeof(h2));
        memcpy(c2, c2_new, sizeof(c2));
    }

    // Dense层1 (ReLU)
    float dense1_out[DENSE1_UNITS];
    dense_forward(dense1_W, h2, dense1_b, dense1_out,
                 LSTM2_UNITS, DENSE1_UNITS, true);

    // Dense层2 (Linear, 输出层)
    float output[N_OUTPUTS];
    dense_forward(dense2_W, dense1_out, dense2_b, output,
                 DENSE1_UNITS, N_OUTPUTS, false);

    // 反归一化
    float soc_f = output[0] * TARG_RANGE[0] + TARG_MIN[0];
    float rm_f = output[1] * TARG_RANGE[1] + TARG_MIN[1];

    if (soc_f < 0.0f) soc_f = 0.0f;
    if (soc_f > 100.0f) soc_f = 100.0f;
    if (rm_f < 0.0f) rm_f = 0.0f;
    if (rm_f > TARG_RANGE[1]) rm_f = TARG_RANGE[1];  // 裁剪到设计容量

    *out_soc = (int)(soc_f + 0.5f);
    *out_rm = (int)(rm_f + 0.5f);

    return true;
}
