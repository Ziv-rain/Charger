#include "ai_soc.h"
#include "soc_model_normal.h"  // 需要替换为Dense模型

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

// ================= 配置 =================
static constexpr int TENSOR_ARENA_SIZE = 32 * 1024;  // 32KB（Dense模型更小）
static constexpr int LOOK_BACK = 10;
static constexpr int N_FEATURES = 4;

// Tensor Arena: 动态分配
static uint8_t* tensor_arena = nullptr;

// TFLite对象
static const tflite::Model* model = nullptr;
static tflite::MicroInterpreter* interpreter = nullptr;
static TfLiteTensor* input_tensor = nullptr;
static TfLiteTensor* output_tensor = nullptr;

// ================= 特征环形缓冲区 =================
static float feature_buffer[LOOK_BACK][N_FEATURES];
static int buffer_index = 0;
static bool buffer_full = false;

// ================= Scaler参数 =================
static const float FEAT_MIN[] = {2994.0f, -477.0f, -477.0f, 20.1f};
static const float FEAT_MAX[] = {4157.0f, 1011.0f,  989.0f, 48.9f};
static const float FEAT_RANGE[] = {
    FEAT_MAX[0] - FEAT_MIN[0],
    FEAT_MAX[1] - FEAT_MIN[1],
    FEAT_MAX[2] - FEAT_MIN[2],
    FEAT_MAX[3] - FEAT_MIN[3]
};

static const float TARG_MIN[] = {0.0f,    0.0f};
static const float TARG_MAX[] = {100.0f, 1500.0f};

// ================= 初始化 =================
bool ai_soc_init() {
    Serial.println("AI SOC (Dense): 正在加载模型...");

    tensor_arena = (uint8_t*)malloc(TENSOR_ARENA_SIZE);
    if (!tensor_arena) {
        Serial.printf("AI SOC: 内存分配失败！需要%d字节\n", TENSOR_ARENA_SIZE);
        return false;
    }
    Serial.printf("AI SOC: 分配%d字节tensor arena OK\n", TENSOR_ARENA_SIZE);

    model = tflite::GetModel(soc_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        Serial.printf("AI SOC: 模型版本不匹配!\n");
        free(tensor_arena);
        tensor_arena = nullptr;
        return false;
    }

    // 注册算子 - Dense模型只需要基本算子，不需要WHILE
    tflite::MicroMutableOpResolver<20>* resolver = new tflite::MicroMutableOpResolver<20>();
    resolver->AddFullyConnected();
    resolver->AddRelu();
    resolver->AddReshape();
    resolver->AddLogistic();
    resolver->AddTanh();
    resolver->AddMul();
    resolver->AddAdd();
    resolver->AddSub();
    resolver->AddDiv();
    resolver->AddMean();
    resolver->AddStridedSlice();
    resolver->AddConcatenation();
    resolver->AddCast();
    resolver->AddPack();
    resolver->AddShape();

    interpreter = new tflite::MicroInterpreter(
        model, *resolver, tensor_arena, TENSOR_ARENA_SIZE);

    TfLiteStatus status = interpreter->AllocateTensors();
    if (status != kTfLiteOk) {
        Serial.println("AI SOC: 张量分配失败！");
        interpreter = nullptr;
        free(tensor_arena);
        tensor_arena = nullptr;
        free(resolver);
        return false;
    }

    input_tensor = interpreter->input(0);
    output_tensor = interpreter->output(0);

    Serial.printf("AI SOC (Dense): 模型加载成功!\n");
    Serial.printf("  输入: [%d, %d, %d]\n",
                  input_tensor->dims->data[0],
                  input_tensor->dims->data[1],
                  input_tensor->dims->data[2]);
    Serial.printf("  输出: [%d, %d]\n",
                  output_tensor->dims->data[0],
                  output_tensor->dims->data[1]);
    Serial.printf("  Arena: %d / %d bytes\n",
                  interpreter->arena_used_bytes(), TENSOR_ARENA_SIZE);

    buffer_index = 0;
    buffer_full = false;
    memset(feature_buffer, 0, sizeof(feature_buffer));

    return true;
}

// ================= 更新特征缓冲区 =================
void ai_soc_update_buffer(int voltage_mV, int current_mA, int avg_current_mA, float temperature_C) {
    float raw[N_FEATURES] = {
        (float)voltage_mV,
        (float)current_mA,
        (float)avg_current_mA,
        temperature_C
    };

    for (int i = 0; i < N_FEATURES; i++) {
        float norm = (raw[i] - FEAT_MIN[i]) / FEAT_RANGE[i];
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        feature_buffer[buffer_index][i] = norm;
    }

    buffer_index++;
    if (buffer_index >= LOOK_BACK) {
        buffer_index = 0;
        buffer_full = true;
    }
}

// ================= 执行推理 =================
bool ai_soc_predict(int* soc_out, int* rm_out) {
    if (!buffer_full || !interpreter || !input_tensor || !output_tensor) {
        return false;
    }

    // 填入输入张量
    float* input_data = input_tensor->data.f;
    for (int t = 0; t < LOOK_BACK; t++) {
        int src_idx = (buffer_index + t) % LOOK_BACK;
        for (int f = 0; f < N_FEATURES; f++) {
            input_data[t * N_FEATURES + f] = feature_buffer[src_idx][f];
        }
    }

    // 推理
    TfLiteStatus status = interpreter->Invoke();
    if (status != kTfLiteOk) {
        Serial.println("AI SOC: 推理失败！");
        return false;
    }

    // 反归一化
    float* output_data = output_tensor->data.f;
    float ai_soc_f = output_data[0] * (TARG_MAX[0] - TARG_MIN[0]) + TARG_MIN[0];
    float ai_rm_f = output_data[1] * (TARG_MAX[1] - TARG_MIN[1]) + TARG_MIN[1];

    ai_soc_f = constrain(ai_soc_f, 0.0f, 100.0f);
    ai_rm_f = constrain(ai_rm_f, 0.0f, 1500.0f);

    *soc_out = (int)(ai_soc_f + 0.5f);
    *rm_out = (int)(ai_rm_f + 0.5f);

    return true;
}
