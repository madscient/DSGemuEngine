// DSGemuEngine.cpp
// FmEngineApi 準拠エミュレーションエンジン
// 統合コア:
//   core/ym2163  YM2163 (DSG: Digital Sound Generator) → DSG

#include "FmEngineApi.h"
#include "../core/ym2163.h"

#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <algorithm>

// =========================================================
//  定数
// =========================================================
// 楽音 1 チャンネルの最大振幅がコア出力の 4096 なので、
// 4 チャンネル同時発音でおよそフルスケールになる
static constexpr float kOutputScale = 1.0f / 16384.0f;

// 楽音波形は Or / Pf / Hc のように直流成分を持つものがあり、
// エンベロープがそれを振幅変調してしまう。実機の出力段と同じく交流結合する
static constexpr float kDcBlockHz = 5.0f;

// =========================================================
//  部位
//  出力端子がそのまま部位になる。添字はコアの YM2163_PIN_* と共通
// =========================================================
static const char* const kPartNames[] = {
    "OR1", "OR2", "OR3", "OR4", "RH1", "RH2",
};
static constexpr uint32_t kPartCount = (uint32_t)(sizeof(kPartNames) / sizeof(kPartNames[0]));

static_assert(kPartCount == YM2163_NUM_PINS &&
              YM2163_PIN_OR1 == 0 && YM2163_PIN_OR2 == 1 && YM2163_PIN_OR3 == 2 &&
              YM2163_PIN_OR4 == 3 && YM2163_PIN_RH1 == 4 && YM2163_PIN_RH2 == 5,
              "kPartNames must follow the YM2163_PIN_* order");

static int findPart(const char* name) {
    if (!name) return -1;
    for (uint32_t i = 0; i < kPartCount; ++i)
        if (strcmp(kPartNames[i], name) == 0)
            return (int)i;
    return -1;
}

// =========================================================
//  チップエントリ
// =========================================================
struct ChipEntry {
    std::string name;
    uint32_t    sample_rate = 0;
    uint32_t    clock       = 0;

    struct Deleter { void operator()(YM2163* p) const { YM2163_delete(p); } };
    std::unique_ptr<YM2163, Deleter> dev;

    // 直流阻止フィルタ。部位ゲインが L/R で違うと入力が別の信号になるので、
    // 状態を L/R ([0] / [1]) で分けて持つ
    float dc_r     = 0.0f;   // 極
    float dc_x1[2] = { 0.0f, 0.0f };
    float dc_y1[2] = { 0.0f, 0.0f };

    float gain_l = 1.0f;
    float gain_r = 1.0f;

    float part_gain_l[kPartCount];
    float part_gain_r[kPartCount];

    ChipEntry() {
        std::fill_n(part_gain_l, kPartCount, 1.0f);
        std::fill_n(part_gain_r, kPartCount, 1.0f);
    }
};

// =========================================================
//  エンジン本体
// =========================================================
struct FmEngineOpaque {
    uint32_t sample_rate;
    std::vector<std::unique_ptr<ChipEntry>> chips;
    std::mutex write_mutex;
};

// =========================================================
//  対応チップテーブル
// =========================================================
struct ChipDesc {
    const char* name;
};

static const ChipDesc kChipTable[] = {
    { "DSG" },  // YM2163
};
static constexpr uint32_t kChipCount = (uint32_t)(sizeof(kChipTable) / sizeof(kChipTable[0]));

static const ChipDesc* findChipDesc(const char* name) {
    if (!name) return nullptr;
    for (uint32_t i = 0; i < kChipCount; ++i)
        if (strcmp(kChipTable[i].name, name) == 0)
            return &kChipTable[i];
    return nullptr;
}

// =========================================================
//  チップ生成
// =========================================================
static std::unique_ptr<ChipEntry> createChip(
    const ChipDesc& desc, uint32_t clock, uint32_t sample_rate)
{
    auto e = std::make_unique<ChipEntry>();
    e->name        = desc.name;
    e->sample_rate = sample_rate;
    e->clock       = clock;

    e->dev.reset(YM2163_new(e->clock, sample_rate));
    if (!e->dev) return nullptr;

    const float r = 1.0f - (2.0f * 3.14159265f * kDcBlockHz / (float)sample_rate);
    e->dc_r = std::max(0.0f, std::min(0.99999f, r));

    return e;
}

// =========================================================
//  1 出力サンプル生成
//  コア側でレート変換済みなので 1 回呼ぶだけでよい
// =========================================================
static void chipCalcStereo(ChipEntry& c, float& out_l, float& out_r) {
    int16_t pins[YM2163_NUM_PINS];
    YM2163_calc_pins(c.dev.get(), pins);

    float x[2] = { 0.0f, 0.0f };
    for (uint32_t p = 0; p < kPartCount; ++p) {
        x[0] += (float)pins[p] * c.part_gain_l[p];
        x[1] += (float)pins[p] * c.part_gain_r[p];
    }

    const float gain[2] = { c.gain_l, c.gain_r };
    float* const out[2] = { &out_l, &out_r };
    for (int ch = 0; ch < 2; ++ch) {
        // コアの YM2163_calc が全端子の合成に掛ける 16 ビットの飽和と同じ。
        // 部位ゲインがすべて 1.0 のとき、合成値が YM2163_calc と一致する
        const float xs = std::max(-32768.0f, std::min(32767.0f, x[ch]));
        const float y  = xs - c.dc_x1[ch] + c.dc_r * c.dc_y1[ch];
        c.dc_x1[ch] = xs;
        c.dc_y1[ch] = y;

        *out[ch] += y * kOutputScale * gain[ch];
    }
}

// =========================================================
//  C API 実装
// =========================================================
extern "C" {

FMENGINE_API FmEngineHandle FMENGINE_CALL FmEngine_Create(uint32_t sample_rate) {
    if (sample_rate == 0) sample_rate = 48000;
    auto* eng = new(std::nothrow) FmEngineOpaque();
    if (!eng) return nullptr;
    eng->sample_rate = sample_rate;
    return eng;
}

FMENGINE_API void FMENGINE_CALL FmEngine_Destroy(FmEngineHandle engine) {
    delete engine;
}

FMENGINE_API uint32_t FMENGINE_CALL FmEngine_Inquiry(FmEngineHandle /*engine*/) {
    return kChipCount;
}

FMENGINE_API const char* FMENGINE_CALL FmEngine_GetSupportedChip(
    FmEngineHandle /*engine*/, uint32_t index)
{
    if (index >= kChipCount) return nullptr;
    return kChipTable[index].name;
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_AddChip(
    FmEngineHandle engine, const char* name, uint32_t clock, uint32_t* out_id)
{
    // コアの YM2163_new は clock=0 を 1MHz に読み替えるので、エンジンが既定の
    // クロックを持たないようにここで止める
    if (!engine || !name || clock == 0) return FM_ERR_INVALID_ARG;
    const ChipDesc* desc = findChipDesc(name);
    if (!desc) return FM_ERR_UNKNOWN_CHIP;

    auto chip = createChip(*desc, clock, engine->sample_rate);
    if (!chip) return FM_ERR_ALLOC;

    if (out_id) *out_id = (uint32_t)engine->chips.size();
    engine->chips.push_back(std::move(chip));
    return FM_OK;
}

FMENGINE_API const char* FMENGINE_CALL FmEngine_GetChipName(
    FmEngineHandle engine, uint32_t chip_id)
{
    if (!engine || chip_id >= engine->chips.size()) return nullptr;
    return engine->chips[chip_id]->name.c_str();
}

FMENGINE_API uint32_t FMENGINE_CALL FmEngine_GetSampleRate(FmEngineHandle engine) {
    if (!engine) return 0;
    return engine->sample_rate;
}

// reg は DSG のアドレス (80H〜9FH)、value は D0〜D6 の 7 ビットデータ。
// アドレスラッチとデータの 2 回書き込みはコアが内部で行う
FMENGINE_API FmResult FMENGINE_CALL FmEngine_Write(
    FmEngineHandle engine, uint32_t chip_id,
    uint8_t reg, uint8_t value, uint32_t /*port*/)
{
    if (!engine || chip_id >= engine->chips.size()) return FM_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(engine->write_mutex);
    YM2163_write_reg(engine->chips[chip_id]->dev.get(), reg, value);
    return FM_OK;
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_SetGain(
    FmEngineHandle engine, uint32_t chip_id, float gain_l, float gain_r)
{
    if (!engine || chip_id >= engine->chips.size()) return FM_ERR_INVALID_ARG;
    engine->chips[chip_id]->gain_l = gain_l;
    engine->chips[chip_id]->gain_r = gain_r;
    return FM_OK;
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_GetGain(
    FmEngineHandle engine, uint32_t chip_id,
    float* out_gain_l, float* out_gain_r)
{
    if (!engine || chip_id >= engine->chips.size()) return FM_ERR_INVALID_ARG;
    if (out_gain_l) *out_gain_l = engine->chips[chip_id]->gain_l;
    if (out_gain_r) *out_gain_r = engine->chips[chip_id]->gain_r;
    return FM_OK;
}

FMENGINE_API uint32_t FMENGINE_CALL FmEngine_GetPartCount(
    FmEngineHandle engine, uint32_t chip_id)
{
    if (!engine || chip_id >= engine->chips.size()) return 0;
    return kPartCount;
}

FMENGINE_API const char* FMENGINE_CALL FmEngine_GetPartName(
    FmEngineHandle engine, uint32_t chip_id, uint32_t index)
{
    if (!engine || chip_id >= engine->chips.size() || index >= kPartCount) return nullptr;
    return kPartNames[index];
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_SetPartGain(
    FmEngineHandle engine, uint32_t chip_id, const char* part,
    float gain_l, float gain_r)
{
    if (!engine || chip_id >= engine->chips.size()) return FM_ERR_INVALID_ARG;
    const int p = findPart(part);
    if (p < 0) return FM_ERR_INVALID_ARG;
    engine->chips[chip_id]->part_gain_l[p] = gain_l;
    engine->chips[chip_id]->part_gain_r[p] = gain_r;
    return FM_OK;
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_GetPartGain(
    FmEngineHandle engine, uint32_t chip_id, const char* part,
    float* out_gain_l, float* out_gain_r)
{
    if (!engine || chip_id >= engine->chips.size()) return FM_ERR_INVALID_ARG;
    const int p = findPart(part);
    if (p < 0) return FM_ERR_INVALID_ARG;
    if (out_gain_l) *out_gain_l = engine->chips[chip_id]->part_gain_l[p];
    if (out_gain_r) *out_gain_r = engine->chips[chip_id]->part_gain_r[p];
    return FM_OK;
}

// 波形は内蔵 ROM だけで外部メモリを持たないので、ヘッダが宣言する外部メモリの
// 関数 (任意の組) は定義しない

FMENGINE_API FmResult FMENGINE_CALL FmEngine_Generate(
    FmEngineHandle engine, float* out_l, float* out_r, uint32_t samples)
{
    if (!engine || !out_l || !out_r) return FM_ERR_INVALID_ARG;

    for (uint32_t i = 0; i < samples; ++i) {
        float l = 0.0f, r = 0.0f;
        {
            std::lock_guard<std::mutex> lock(engine->write_mutex);
            for (auto& chip : engine->chips)
                chipCalcStereo(*chip, l, r);
        }
        out_l[i] = std::max(-1.0f, std::min(1.0f, l));
        out_r[i] = std::max(-1.0f, std::min(1.0f, r));
    }
    return FM_OK;
}

} // extern "C"
