#ifndef YM2163_H
#define YM2163_H
/* YM2163 (DSG: Digital Sound Generator) エミュレーションコア
 *
 * 単体で利用可能。C99 と標準ライブラリ (malloc/free) 以外に依存しない。
 *
 *   YM2163 *chip = YM2163_new(1000000, 48000);
 *   YM2163_reset(chip);
 *   YM2163_write_reg(chip, 0x80, 0x7F);
 *   int16_t s = YM2163_calc(chip);
 *   YM2163_delete(chip);
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 出力端子: OR1〜OR4 (楽音) / RH1, RH2 (リズム音) */
enum {
    YM2163_PIN_OR1 = 0,
    YM2163_PIN_OR2,
    YM2163_PIN_OR3,
    YM2163_PIN_OR4,
    YM2163_PIN_RH1,
    YM2163_PIN_RH2,
    YM2163_NUM_PINS
};

#define YM2163_NUM_MELODY   4
#define YM2163_NUM_RHYTHM   4   /* HH, BD, HC, SDN */
#define YM2163_DEFAULT_CLOCK 1000000

/* 波形メモリ選択 (W3W2W1) */
enum {
    YM2163_WAVE_NONE0 = 0,
    YM2163_WAVE_ST    = 1,  /* ストリングス */
    YM2163_WAVE_OR    = 2,  /* オルガン */
    YM2163_WAVE_CL    = 3,  /* クラリネット */
    YM2163_WAVE_PF    = 4,  /* ピアノ */
    YM2163_WAVE_HC    = 5   /* ハープシコード */
};

/* リズム音の並びはレジスタ 94H〜97H の順 */
enum {
    YM2163_RHYTHM_HH = 0,   /* ハイハット (HHO / HHD 共用) */
    YM2163_RHYTHM_BD,       /* バスドラム */
    YM2163_RHYTHM_HC,       /* ハイコンガ */
    YM2163_RHYTHM_SDN       /* スネアドラムノイズ */
};

typedef struct YM2163 YM2163;

YM2163 *YM2163_new(uint32_t clock, uint32_t rate);
void    YM2163_delete(YM2163 *chip);

/* IC 端子のロー入力と同じ。全レジスタを 0 にクリアする */
void    YM2163_reset(YM2163 *chip);

void    YM2163_set_clock(YM2163 *chip, uint32_t clock);
/* rate = 0 でネイティブレート (clock/4) 出力になる */
void    YM2163_set_rate(YM2163 *chip, uint32_t rate);
uint32_t YM2163_native_rate(const YM2163 *chip);

/* 実チップのバスプロトコル。D7=1 でアドレスラッチ、D7=0 で直前のアドレスへ D0〜D6 を書き込む */
void    YM2163_write_bus(YM2163 *chip, uint8_t data);
/* アドレスとデータを一度に与える簡易形式。addr/data とも下位 7 ビットのみ有効 */
void    YM2163_write_reg(YM2163 *chip, uint8_t addr, uint8_t data);

/* リードモードのデータバス。D0 にタイマーフラグが出る */
uint8_t YM2163_read(const YM2163 *chip);
/* IRQ 端子の論理。1 = 割り込み要求中 (端子はローレベル) */
int     YM2163_irq(const YM2163 *chip);

/* 出力レートで 1 サンプル。全端子の合成値 */
int16_t YM2163_calc(YM2163 *chip);
/* 出力レートで 1 サンプル。端子ごとの内訳を out[YM2163_NUM_PINS] に返す */
void    YM2163_calc_pins(YM2163 *chip, int16_t *out);

/* ネイティブレート (clock/4) で 1 ステップ進める。レート変換を自前で行う場合に使う */
int16_t YM2163_calc_native(YM2163 *chip);
void    YM2163_calc_native_pins(YM2163 *chip, int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* YM2163_H */
