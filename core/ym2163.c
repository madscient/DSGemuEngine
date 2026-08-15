/* YM2163 (DSG: Digital Sound Generator) エミュレーションコア */

#include "ym2163.h"

#include <stdlib.h>
#include <string.h>

/* 位相は 5.27 固定小数。uint32_t が 32 アドレスちょうどで一周する */
#define PHASE_BITS      27
#define PHASE_ADDR(p)   ((p) >> PHASE_BITS)

/* エンベロープは線形。EG_FRAC は 1 ネイティブステップあたりの変化量の小数部 */
#define EG_BITS         24
#define EG_MAX          (1 << EG_BITS)
#define EG_FRAC         8
#define EG_FRAC_MASK    ((1 << EG_FRAC) - 1)

/* 各フェーズ長はマスタークロック 1MHz 基準のネイティブステップ数で保持する。
   実チップと同じくクロックを変えれば実時間も比例して変わる */
#define NATIVE_1MHZ     250000
#define TICKS_MS(ms)    ((int32_t)((ms) * (NATIVE_1MHZ / 1000)))

/* =======================================================================
 *  波形メモリ (図1)
 *
 *  データシートの図をトレースして得た 6 ビット符号付きの値。±1 程度の
 *  読み取り誤差を含む可能性がある。W3W2W1 = 000/110/111 は未定義で無音。
 * ======================================================================= */
static const int8_t WAVE_ROM[8][32] = {
    /* 000 未定義 */
    { 0 },
    /* 001 St (ストリングス): -31 から +31 まで 2 きざみののこぎり波 */
    { -31, -29, -27, -25, -23, -21, -19, -17, -15, -13, -11,  -9,  -7,  -5,  -3,  -1,
        1,   3,   5,   7,   9,  11,  13,  15,  17,  19,  21,  23,  25,  27,  29,  31 },
    /* 010 Or (オルガン) */
    { -20, -20, -20, -20, -20, -20, -20, -20,   4,   4,   4,   4,   4,   4,   4,   4,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 },
    /* 011 Cl (クラリネット) */
    { -25, -25, -25, -25, -25, -25, -25, -25, -25, -25, -25, -25, -25, -25, -25, -25,
       25,  25,  25,  25,  25,  25,  25,  25,  25,  25,  25,  25,  25,  25,  25,  25 },
    /* 100 Pf (ピアノ) */
    { -30, -30, -30, -30, -30, -30, -30, -30,  12,  12,  12,  12,  12,  12,  12,  12,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 },
    /* 101 Hc (ハープシコード) */
    { -30, -30,  -5,  -5,   0,   0,   5,   5,  10,  10,  31,  31,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 },
    /* 110 未定義 */
    { 0 },
    /* 111 未定義 */
    { 0 },
};

/* リズムオシレータの音程成分。分周器出力そのままの矩形波 */
#define RHYTHM_TONE_AMP  31
static int32_t rhythm_tone(uint32_t phase) {
    return (phase & 0x80000000u) ? RHYTHM_TONE_AMP : -RHYTHM_TONE_AMP;
}

/* =======================================================================
 *  楽音チャンネル
 * ======================================================================= */
enum { EG_OFF = 0, EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

typedef struct {
    /* レジスタ */
    uint16_t div;       /* DV7〜DV1/4 の 10 ビット (LSB = 1/4) */
    uint8_t  oct;       /* B2B1: 0..3 = オクターブ 1..4 */
    uint8_t  kon;
    uint8_t  fd;        /* フォーシングダンプ */
    uint8_t  wave;      /* W3W2W1 */
    uint8_t  env_sel;   /* E2E1 */
    uint8_t  sus;
    uint8_t  vol;       /* VL2VL1: 0..3 = 0 / -6 / -12 / -inf dB */
    uint8_t  pins;      /* F4F3F2F1 */

    /* 実行状態 */
    uint32_t phase;
    uint32_t phase_inc;
    uint8_t  eg_state;
    int32_t  eg_level;
    int32_t  eg_rate;
    int32_t  eg_frac;
} Melody;

/* =======================================================================
 *  リズム音
 * ======================================================================= */
typedef struct {
    uint8_t  level;     /* LV4〜LV0: 0 が最大音量、31 が最小音量 */
    uint8_t  hold;      /* LH */
    uint8_t  active;
    int32_t  level_fp;  /* level を 16 ビット小数付きで保持したもの */
    int32_t  decay;     /* 1 ネイティブステップあたりの level_fp 増分 */
    uint32_t phase;     /* 音程を持つ音 (BD/HC) 用 */
    uint32_t phase_inc;
} Rhythm;

#define RLVL_FRAC   16
#define RLVL_MUTE   (32 << RLVL_FRAC)

/* =======================================================================
 *  チップ
 * ======================================================================= */
struct YM2163 {
    uint32_t clock;
    uint32_t rate;
    uint32_t native_rate;

    uint8_t  reg[0x20];     /* アドレス 80H〜9FH のミラー */
    uint8_t  latch;         /* D7=1 で書き込まれたアドレス */

    Melody   mel[YM2163_NUM_MELODY];
    Rhythm   rhy[YM2163_NUM_RHYTHM];

    /* ノイズ源 */
    uint32_t noise;
    int32_t  noise_cnt_sd;
    int32_t  noise_cnt_hh;
    int32_t  noise_out_sd;
    int32_t  noise_out_hh;

    /* タイマー */
    uint16_t timer_period;
    int32_t  timer_cnt;
    uint8_t  timer_flag;
    uint8_t  irq_enable;

    /* レート変換 (integrate & dump) */
    double   resample_step;
    double   tick_remain;
    int32_t  tick_value[YM2163_NUM_PINS];
};

/* =======================================================================
 *  楽音: 位相とエンベロープ
 * ======================================================================= */
static int32_t eg_rate_for(int32_t delta, int32_t ticks) {
    int64_t r;
    if (ticks <= 0 || delta <= 0) return EG_MAX;   /* 256 ステップで全域を走る */
    r = ((int64_t)delta << EG_FRAC) / ticks;
    return (r < 1) ? 1 : (int32_t)r;
}

static void melody_update_phase_inc(Melody *m) {
    if (m->div == 0) {
        m->phase_inc = 0;
        return;
    }
    /* 1 ネイティブステップ (clock/4) あたりの波形メモリアドレス進み量は
       128 / (DV * 2^(3-oct))。DV は 1/4 きざみの生値なのでそのまま使える */
    m->phase_inc = (uint32_t)(((uint64_t)1 << (PHASE_BITS + 7)) /
                              ((uint64_t)m->div << (3 - m->oct)));
}

static void melody_key_on(Melody *m) {
    m->phase   = 0;
    m->eg_frac = 0;
    switch (m->env_sel) {
    case 1:     /* エンベロープ 1: 60ms の直線アタック */
        m->eg_state = EG_ATTACK;
        m->eg_level = 0;
        m->eg_rate  = eg_rate_for(EG_MAX, TICKS_MS(60));
        break;
    case 3:     /* エンベロープ 3: 立ち上がり即最大、以後保持 */
        m->eg_state = EG_SUSTAIN;
        m->eg_level = EG_MAX;
        m->eg_rate  = 0;
        break;
    default:    /* エンベロープ 0 / 2: 立ち上がり即最大、60ms で 1/2 まで減衰 */
        m->eg_state = EG_DECAY;
        m->eg_level = EG_MAX;
        m->eg_rate  = eg_rate_for(EG_MAX / 2, TICKS_MS(60));
        break;
    }
}

static void melody_silence(Melody *m) {
    m->eg_state = EG_OFF;
    m->eg_level = 0;
    m->eg_rate  = 0;
}

static void melody_key_off(Melody *m) {
    int32_t ticks;
    if (m->eg_state == EG_OFF || m->eg_level <= 0) {
        melody_silence(m);
        return;
    }
    if (m->env_sel == 0 && m->sus) return;   /* 図2: KOFF が効かない唯一の組み合わせ */
    if (m->env_sel == 3 && !m->sus) {
        melody_silence(m);
        return;
    }

    if (m->sus) {
        ticks = TICKS_MS(1200);
    } else {
        ticks = (m->env_sel == 1) ? TICKS_MS(120) : TICKS_MS(60);
    }
    m->eg_state = EG_RELEASE;
    m->eg_rate  = eg_rate_for(m->eg_level, ticks);
    m->eg_frac  = 0;
}

/* フォーシングダンプ。データシートに時定数の規定はないため実用的な値を置く */
static void melody_damp(Melody *m) {
    if (m->eg_state == EG_OFF || m->eg_level <= 0) {
        melody_silence(m);
        return;
    }
    m->eg_state = EG_RELEASE;
    m->eg_rate  = eg_rate_for(m->eg_level, TICKS_MS(5));
    m->eg_frac  = 0;
}

static void melody_eg_step(Melody *m) {
    int32_t d;
    if (m->eg_rate == 0) return;

    m->eg_frac += m->eg_rate;
    d = m->eg_frac >> EG_FRAC;
    m->eg_frac &= EG_FRAC_MASK;
    if (d == 0) return;

    switch (m->eg_state) {
    case EG_ATTACK:
        m->eg_level += d;
        if (m->eg_level >= EG_MAX) {
            m->eg_level = EG_MAX;
            m->eg_state = EG_SUSTAIN;
            m->eg_rate  = 0;
        }
        break;

    case EG_DECAY:
        m->eg_level -= d;
        if (m->eg_level <= EG_MAX / 2) {
            m->eg_level = EG_MAX / 2;
            m->eg_state = EG_SUSTAIN;
            /* エンベロープ 0 だけは 1/2 到達後も 1.2 秒かけて 0 まで落ちる */
            m->eg_rate = (m->env_sel == 0)
                       ? eg_rate_for(EG_MAX / 2, TICKS_MS(1200)) : 0;
            m->eg_frac = 0;
        }
        break;

    case EG_SUSTAIN:
    case EG_RELEASE:
        m->eg_level -= d;
        if (m->eg_level <= 0) melody_silence(m);
        break;

    default:
        break;
    }
}

static int32_t melody_output(const Melody *m) {
    int32_t wave, amp, v;
    if (m->eg_state == EG_OFF || m->vol == 3) return 0;

    wave = WAVE_ROM[m->wave][PHASE_ADDR(m->phase)];
    amp  = m->eg_level >> (EG_BITS - 12);        /* 0..4096 */
    v    = (wave * amp) >> 5;                    /* 波形の振幅 32 を基準に正規化 */
    return v >> m->vol;                          /* 0 / -6 / -12 dB */
}

/* =======================================================================
 *  リズム音
 * ======================================================================= */
/* 減衰時間・音程はデータシートに規定がない。実機の音に合わせた近似値 */
static const int32_t RHYTHM_DECAY_MS[YM2163_NUM_RHYTHM] = {
    45,     /* HH クローズ。オープンはトリガー時に差し替える */
    250,    /* BD */
    180,    /* HC */
    130     /* SDN */
};
#define HH_OPEN_DECAY_MS  300
#define BD_FREQ_HZ        100
#define HC_FREQ_HZ        260
#define SDN_FREQ_HZ       300

static int32_t rhythm_decay_rate(int32_t ms) {
    int64_t ticks = (int64_t)ms * (NATIVE_1MHZ / 1000);
    if (ticks <= 0) return RLVL_MUTE;
    return (int32_t)(RLVL_MUTE / ticks) + 1;
}

static void rhythm_trigger(YM2163 *chip, int idx, int open_hh) {
    Rhythm *r = &chip->rhy[idx];
    int32_t ms = RHYTHM_DECAY_MS[idx];
    if (idx == YM2163_RHYTHM_HH && open_hh) ms = HH_OPEN_DECAY_MS;

    r->active = 1;
    r->phase  = 0;
    r->decay  = rhythm_decay_rate(ms);
    /* LH=1 は外部からレベルを与えるモードなので内蔵エンベロープで上書きしない */
    if (!r->hold) r->level_fp = 0;
}

static void rhythm_step(Rhythm *r) {
    if (!r->active) return;
    r->phase += r->phase_inc;
    if (r->hold) return;

    r->level_fp += r->decay;
    if (r->level_fp >= RLVL_MUTE) {
        r->level_fp = RLVL_MUTE;
        r->active   = 0;
    }
}

static int32_t rhythm_amp(const Rhythm *r) {
    int32_t lv;
    if (!r->active) return 0;
    lv = r->hold ? ((int32_t)r->level << RLVL_FRAC) : r->level_fp;
    if (lv >= RLVL_MUTE) return 0;
    /* リズムレベル 0 が最大音量、31 が最小音量 */
    return (RLVL_MUTE - lv) >> (RLVL_FRAC - 7);   /* 0..4096 */
}

static void noise_step(YM2163 *chip) {
    /* ノイズの帯域はデータシートに規定がない。スネアよりハイハットの
       更新レートを上げて音色差をつける */
    if (--chip->noise_cnt_sd <= 0) {
        chip->noise_cnt_sd = 12;
        chip->noise = (chip->noise >> 1) |
                      (((chip->noise ^ (chip->noise >> 5)) & 1) << 22);
        chip->noise_out_sd = (chip->noise & 1) ? 31 : -31;
    }
    if (--chip->noise_cnt_hh <= 0) {
        chip->noise_cnt_hh = 6;
        chip->noise = (chip->noise >> 1) |
                      (((chip->noise ^ (chip->noise >> 5)) & 1) << 22);
        chip->noise_out_hh = (chip->noise & 1) ? 31 : -31;
    }
}

static void update_rhythm_pitch(YM2163 *chip) {
    uint64_t nr = chip->native_rate ? chip->native_rate : 1;
    chip->rhy[YM2163_RHYTHM_BD].phase_inc =
        (uint32_t)(((uint64_t)BD_FREQ_HZ << 32) / nr);
    chip->rhy[YM2163_RHYTHM_HC].phase_inc =
        (uint32_t)(((uint64_t)HC_FREQ_HZ << 32) / nr);
    chip->rhy[YM2163_RHYTHM_SDN].phase_inc =
        (uint32_t)(((uint64_t)SDN_FREQ_HZ << 32) / nr);
}

/* =======================================================================
 *  レジスタ書き込み
 * ======================================================================= */
static void write_register(YM2163 *chip, uint8_t addr, uint8_t data) {
    data &= 0x7F;
    if (addr < 0x80) return;
    chip->reg[addr - 0x80] = data;

    if (addr <= 0x83) {                     /* 分周数 (下位) */
        Melody *m = &chip->mel[addr - 0x80];
        m->div = (uint16_t)((m->div & 0x380) | data);
        melody_update_phase_inc(m);

    } else if (addr <= 0x87) {              /* KON / FD / オクターブ / 分周数 (上位) */
        Melody *m = &chip->mel[addr - 0x84];
        uint8_t kon = (uint8_t)((data >> 6) & 1);
        uint8_t fd  = (uint8_t)((data >> 5) & 1);
        m->div = (uint16_t)((m->div & 0x07F) | ((data & 0x07) << 7));
        m->oct = (uint8_t)((data >> 3) & 3);
        melody_update_phase_inc(m);

        if (kon && !m->kon)      melody_key_on(m);
        else if (!kon && m->kon) melody_key_off(m);
        m->kon = kon;

        if (fd && !m->fd) melody_damp(m);
        m->fd = fd;

    } else if (addr <= 0x8B) {              /* エンベロープ / サスティン / 波形 */
        Melody *m = &chip->mel[addr - 0x88];
        m->env_sel = (uint8_t)((data >> 5) & 3);
        m->sus     = (uint8_t)((data >> 4) & 1);
        m->wave    = (uint8_t)(data & 0x07);

    } else if (addr <= 0x8F) {              /* 音量 / 出力端子選択 */
        Melody *m = &chip->mel[addr - 0x8C];
        m->vol  = (uint8_t)((data >> 4) & 3);
        m->pins = (uint8_t)(data & 0x0F);

    } else if (addr <= 0x93) {              /* リズムトリガー / 割り込み制御 */
        chip->irq_enable = (uint8_t)((data >> 5) & 1);
        if (data & 0x40) chip->timer_flag = 0;                       /* FGR */
        if (data & 0x01) rhythm_trigger(chip, YM2163_RHYTHM_BD,  0);
        if (data & 0x02) rhythm_trigger(chip, YM2163_RHYTHM_HC,  0);
        if (data & 0x04) rhythm_trigger(chip, YM2163_RHYTHM_SDN, 0);
        if (data & 0x08) rhythm_trigger(chip, YM2163_RHYTHM_HH,  1); /* HHO */
        if (data & 0x10) rhythm_trigger(chip, YM2163_RHYTHM_HH,  0); /* HHD */
        /* トリガーと FGR は一定時間後に 0 へ戻る (表2 の * 印) */
        chip->reg[addr - 0x80] = (uint8_t)(data & 0x20);

    } else if (addr <= 0x97) {              /* リズムレベル */
        Rhythm *r = &chip->rhy[addr - 0x94];
        r->level    = (uint8_t)((data >> 1) & 0x1F);
        r->hold     = (uint8_t)(data & 1);
        r->level_fp = (int32_t)r->level << RLVL_FRAC;

    } else if (addr <= 0x9B) {              /* タイマー周期 (下位) */
        chip->timer_period = (uint16_t)((chip->timer_period & 0x3F80) | data);

    } else if (addr <= 0x9F) {              /* タイマー周期 (上位) */
        chip->timer_period = (uint16_t)((chip->timer_period & 0x007F) | (data << 7));
    }
}

/* =======================================================================
 *  公開 API
 * ======================================================================= */
YM2163 *YM2163_new(uint32_t clock, uint32_t rate) {
    YM2163 *chip = (YM2163 *)calloc(1, sizeof(YM2163));
    if (!chip) return NULL;
    chip->clock       = clock ? clock : YM2163_DEFAULT_CLOCK;
    chip->native_rate = chip->clock / 4;
    YM2163_set_rate(chip, rate);
    YM2163_reset(chip);
    return chip;
}

void YM2163_delete(YM2163 *chip) {
    free(chip);
}

void YM2163_reset(YM2163 *chip) {
    int i;
    if (!chip) return;

    memset(chip->reg, 0, sizeof(chip->reg));
    memset(chip->mel, 0, sizeof(chip->mel));
    memset(chip->rhy, 0, sizeof(chip->rhy));
    chip->latch = 0x80;

    for (i = 0; i < YM2163_NUM_RHYTHM; ++i)
        chip->rhy[i].level_fp = RLVL_MUTE;

    chip->noise        = 0x1234;
    chip->noise_cnt_sd = 1;
    chip->noise_cnt_hh = 1;
    chip->noise_out_sd = 31;
    chip->noise_out_hh = 31;

    chip->timer_period = 0;
    chip->timer_cnt    = 0;
    chip->timer_flag   = 0;
    chip->irq_enable   = 0;

    chip->tick_remain = 0.0;
    memset(chip->tick_value, 0, sizeof(chip->tick_value));

    update_rhythm_pitch(chip);
}

void YM2163_set_clock(YM2163 *chip, uint32_t clock) {
    int i;
    if (!chip) return;
    chip->clock       = clock ? clock : YM2163_DEFAULT_CLOCK;
    chip->native_rate = chip->clock / 4;
    YM2163_set_rate(chip, chip->rate);
    update_rhythm_pitch(chip);
    for (i = 0; i < YM2163_NUM_MELODY; ++i)
        melody_update_phase_inc(&chip->mel[i]);
}

void YM2163_set_rate(YM2163 *chip, uint32_t rate) {
    if (!chip) return;
    chip->rate          = rate ? rate : chip->native_rate;
    chip->resample_step = (double)chip->native_rate / (double)chip->rate;
    chip->tick_remain   = 0.0;
}

uint32_t YM2163_native_rate(const YM2163 *chip) {
    return chip ? chip->native_rate : 0;
}

void YM2163_write_bus(YM2163 *chip, uint8_t data) {
    if (!chip) return;
    if (data & 0x80) chip->latch = data;
    else             write_register(chip, chip->latch, data);
}

void YM2163_write_reg(YM2163 *chip, uint8_t addr, uint8_t data) {
    if (!chip) return;
    write_register(chip, (uint8_t)(addr | 0x80), data);
}

uint8_t YM2163_read(const YM2163 *chip) {
    return chip ? chip->timer_flag : 0;
}

int YM2163_irq(const YM2163 *chip) {
    return (chip && chip->irq_enable && chip->timer_flag) ? 1 : 0;
}

/* =======================================================================
 *  波形生成
 * ======================================================================= */
static void timer_step(YM2163 *chip) {
    /* タイマー周期 PT = (1 + PT値) * 28 / clock 秒。
       ネイティブステップは clock/4 きざみなので (1 + PT値) * 7 ステップ */
    if (--chip->timer_cnt > 0) return;
    chip->timer_cnt  = ((int32_t)chip->timer_period + 1) * 7;
    chip->timer_flag = 1;
}

void YM2163_calc_native_pins(YM2163 *chip, int16_t *out) {
    int32_t acc[YM2163_NUM_PINS] = { 0, 0, 0, 0, 0, 0 };
    int i, p;

    timer_step(chip);
    noise_step(chip);

    for (i = 0; i < YM2163_NUM_MELODY; ++i) {
        Melody *m = &chip->mel[i];
        int32_t v;
        m->phase += m->phase_inc;
        melody_eg_step(m);
        if (!m->pins) continue;
        v = melody_output(m);
        if (!v) continue;
        /* 出力端子はチャンネルごとに独立して選べる。複数選べば
           ソースフォロアが端子の数だけ並列に出る */
        for (p = 0; p < 4; ++p)
            if (m->pins & (1 << p)) acc[p] += v;
    }

    for (i = 0; i < YM2163_NUM_RHYTHM; ++i)
        rhythm_step(&chip->rhy[i]);

    /* BD / HC は矩形波のブリップ音 */
    acc[YM2163_PIN_RH1] +=
        (rhythm_tone(chip->rhy[YM2163_RHYTHM_BD].phase) *
         rhythm_amp(&chip->rhy[YM2163_RHYTHM_BD])) >> 5;
    acc[YM2163_PIN_RH1] +=
        (rhythm_tone(chip->rhy[YM2163_RHYTHM_HC].phase) *
         rhythm_amp(&chip->rhy[YM2163_RHYTHM_HC])) >> 5;

    /* SDN は矩形波のブリップ音にノイズを乗せたもの。両者を等分に混ぜる */
    acc[YM2163_PIN_RH2] +=
        ((rhythm_tone(chip->rhy[YM2163_RHYTHM_SDN].phase) + chip->noise_out_sd) *
         rhythm_amp(&chip->rhy[YM2163_RHYTHM_SDN])) >> 6;

    acc[YM2163_PIN_RH2] +=
        (chip->noise_out_hh * rhythm_amp(&chip->rhy[YM2163_RHYTHM_HH])) >> 5;

    for (p = 0; p < YM2163_NUM_PINS; ++p) {
        int32_t v = acc[p];
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        out[p] = (int16_t)v;
    }
}

int16_t YM2163_calc_native(YM2163 *chip) {
    int16_t pins[YM2163_NUM_PINS];
    int32_t sum = 0;
    int p;
    YM2163_calc_native_pins(chip, pins);
    for (p = 0; p < YM2163_NUM_PINS; ++p) sum += pins[p];
    if (sum >  32767) sum =  32767;
    if (sum < -32768) sum = -32768;
    return (int16_t)sum;
}

void YM2163_calc_pins(YM2163 *chip, int16_t *out) {
    double need = chip->resample_step;
    double acc[YM2163_NUM_PINS] = { 0, 0, 0, 0, 0, 0 };
    int p;

    while (need > 0.0) {
        double take;
        if (chip->tick_remain <= 0.0) {
            int16_t pins[YM2163_NUM_PINS];
            YM2163_calc_native_pins(chip, pins);
            for (p = 0; p < YM2163_NUM_PINS; ++p) chip->tick_value[p] = pins[p];
            chip->tick_remain = 1.0;
        }
        take = (need < chip->tick_remain) ? need : chip->tick_remain;
        for (p = 0; p < YM2163_NUM_PINS; ++p)
            acc[p] += (double)chip->tick_value[p] * take;
        chip->tick_remain -= take;
        need -= take;
    }

    for (p = 0; p < YM2163_NUM_PINS; ++p) {
        double v = acc[p] / chip->resample_step;
        if (v >  32767.0) v =  32767.0;
        if (v < -32768.0) v = -32768.0;
        out[p] = (int16_t)v;
    }
}

int16_t YM2163_calc(YM2163 *chip) {
    int16_t pins[YM2163_NUM_PINS];
    int32_t sum = 0;
    int p;
    YM2163_calc_pins(chip, pins);
    for (p = 0; p < YM2163_NUM_PINS; ++p) sum += pins[p];
    if (sum >  32767) sum =  32767;
    if (sum < -32768) sum = -32768;
    return (int16_t)sum;
}
