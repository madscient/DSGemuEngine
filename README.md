# DSGemuEngine

**FmEngineApi** 準拠の音源エミュレーションエンジン。
YAMAHA **YM2163 (DSG: Digital Sound Generator)** を実装した共有ライブラリ (DLL / .so / .dylib) です。

チップエミュレーション部 (`core/`) は FmEngineApi に依存せず、C99 の標準ライブラリのみで
動作するため、単体で他プロジェクトへ組み込めます。

## YM2163 とは

波形メモリ方式の NMOS 音源 LSI。24 ピン DIP、標準クロック 1MHz、7 ビット D/A コンバータ内蔵。
内蔵の 23 バイトレジスタアレイに設定するだけで、プリセットされた 5 種類の楽音と
4 種類のリズム音を発生します。

| ブロック | 内容 |
|---|---|
| フェイズジェネレータ | 楽音の音程をつくる分周器 |
| ウェーブ ROM | 楽音の波形 (32 ワード × 5 音色) |
| エンベロープジェネレータ | 楽音のエンベロープ (4 種 × サスティン 2 通り) |
| マルチプライヤ | 波形にエンベロープを乗算 |
| リズムオシレータ / リズムエンベロープジェネレータ | リズム音 |
| タイマー | 14 ビット。周期的に割り込みフラグをセット |

## 対応チップ一覧

| チップ名 | 実チップ | ネイティブレート |
|---|---|---|
| `DSG` | YM2163 | clock / 4 (1 MHz で 250,000 Hz) |

ネイティブレートはマスタークロックの 1/4 (楽音 4 チャンネルのマルチプレックス周期) です。

クロックは `FmEngine_AddChip` の `clock` 引数で必ず指定します。エンジンは既定の
クロックを持たず、0 を渡すと `FM_ERR_INVALID_ARG` を返します。

## ファイル構成

```
DSGemuEngine/
├── CMakeLists.txt
├── README.md
├── core/                     ← 単体で分離可能なチップコア
│   ├── ym2163.h
│   └── ym2163.c
├── patches/
│   └── dsg.json              ← FMEngineTest 用パッチ
└── src/
    ├── FmEngineApi.h         ← API ヘッダ (FmEngineApi 共通)
    └── DSGemuEngine.cpp      ← エンジン実装
```

## ビルド

### Linux / macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
# 成果物: build/bin/libDSGemuEngine.so  (Linux)
#         build/bin/libDSGemuEngine.dylib (macOS)
```

### Windows (Visual Studio 2022)

```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
:: 成果物: build\bin\Release\DSGemuEngine.dll
```

チップコア単体のスタティックライブラリ (`ym2163`) も同時に生成されます。

## FMEngineTest との接続

ビルドした共有ライブラリを FMEngineTest の実行ディレクトリに置き、
`-e` オプションで指定します。

```bash
cp build/bin/libDSGemuEngine.so <FMEngineTest_dir>/
cd <FMEngineTest_dir>
./FMEngineTest -e ./libDSGemuEngine.so patches/dsg.json
```

`patches/dsg.json` は `clock` に 1,000,000 Hz を指定しており、レジスタ値はこの
クロックを前提に書いてあります。

## レジスタマップ

`FmEngine_Write(engine, chip_id, reg, val, port)` の `reg` に下表のアドレス、
`val` に 7 ビットのデータを与えます。実チップのアドレスラッチとデータの
2 回書き込みはコアが内部で行うため、アプリケーション側では意識不要です。
`port` は使用しません。

添字 `(0)`〜`(3)` は発音チャンネル番号です。

### 楽音 (オーケストラ音)

| アドレス | D6 | D5 | D4 | D3 | D2 | D1 | D0 |
|---|---|---|---|---|---|---|---|
| `80H`–`83H` | DV4 | DV3 | DV2 | DV1 | DV0 | DV½ | DV¼ |
| `84H`–`87H` | KON | FD | B2 | B1 | DV7 | DV6 | DV5 |
| `88H`–`8BH` | E2 | E1 | SUS | — | W3 | W2 | W1 |
| `8CH`–`8FH` | TEST | VL2 | VL1 | F4 | F3 | F2 | F1 |

**発音周波数**

```
f = clock / (DV * 2^(3 - oct))      oct = B2*2 + B1  (0..3 = オクターブ 1..4)
```

`DV` は DV7〜DV¼ を並べた 10 ビット整数 (LSB = DV¼)。
`clock` = 1MHz、`DV` = 1023、`oct` = 0 のとき 122.19Hz が最低音です。

**キイオン / フォーシングダンプ**

| ビット | 0 | 1 |
|---|---|---|
| KON | キイオフ (減衰開始) | キイオン (発音開始) |
| FD | — | フォーシングダンプ (KON と無関係に強制減衰) |

**波形メモリ (W3W2W1)**

| 値 | 音色 |
|---|---|
| `001` | St (ストリングス) |
| `010` | Or (オルガン) |
| `011` | Cl (クラリネット) |
| `100` | Pf (ピアノ) |
| `101` | Hc (ハープシコード) |

`000` / `110` / `111` は未定義で無音になります。

**エンベロープ (E2E1) とサスティン (SUS)**

| E2E1 | SUS=0 | SUS=1 |
|---|---|---|
| `00` | 即最大 → 60ms で 1/2 → 1.2s で減衰、KOFF で 60ms リリース | 同左。KOFF は効かない |
| `01` | 60ms アタック → 保持、KOFF で 120ms リリース | 60ms アタック → 保持、KOFF で 1.2s リリース |
| `10` | 即最大 → 60ms で 1/2 → 保持、KOFF で 60ms リリース | 同左。KOFF で 1.2s リリース |
| `11` | 即最大 → 保持、KOFF で即無音 (ゲート) | 即最大 → 保持、KOFF で 1.2s リリース |

各時定数はマスタークロックに比例します (上表は 1MHz 時)。

**音量 (VL2VL1) と出力端子選択 (F1〜F4)**

| VL2VL1 | 音量 |
|---|---|
| `00` | 0 dB |
| `01` | −6 dB |
| `10` | −12 dB |
| `11` | −∞ dB |

F1〜F4 はそれぞれ OR1〜OR4 端子への出力可否です。チャンネルごとに独立して
選択でき、複数の端子を同時に選ぶこともできます。どれも選ばないと無音です。

### リズム音

| アドレス | D6 | D5 | D4 | D3 | D2 | D1 | D0 |
|---|---|---|---|---|---|---|---|
| `90H`–`93H` | FGR | IEN | HHD | HHO | SDN | HC | BD |
| `94H`–`97H` | — | LV4 | LV3 | LV2 | LV1 | LV0 | LH |

`90H`〜`93H` は同一レジスタのミラーです。トリガービットに `1` を書くと発音し、
自動的に `0` へ戻ります。

| ビット | 音 | 出力端子 |
|---|---|---|
| BD | バスドラム | RH1 |
| HC | ハイコンガ | RH1 |
| SDN | スネアドラムノイズ | RH2 |
| HHO | ハイハット (オープン) | RH2 |
| HHD | ハイハット (クローズ) | RH2 |

`94H`〜`97H` はそれぞれ HH / BD / HC / SDN のレベルです。LV4〜LV0 は
リズムレベル 0 が最大音量、31 が最小音量。LH = 1 で設定したレベルを保持し、
内蔵リズムエンベロープジェネレータによらず外部からレベルを制御できます。

### タイマー

| アドレス | D6 | D5 | D4 | D3 | D2 | D1 | D0 |
|---|---|---|---|---|---|---|---|
| `98H`–`9BH` | PT6 | PT5 | PT4 | PT3 | PT2 | PT1 | PT0 |
| `9CH`–`9FH` | PT13 | PT12 | PT11 | PT10 | PT9 | PT8 | PT7 |

```
タイマー周期 PT = (1 + PT値) * 28 / clock   [秒]
```

周期ごとに割り込みフラグがセットされます。`90H` の FGR に `1` を書くとフラグが
リセットされ、IEN = 1 のときフラグの状態が IRQ 端子に反転出力されます。
FmEngineApi にはフラグ読み出しの経路がないため、タイマーはコア API
(`YM2163_read` / `YM2163_irq`) からのみ参照できます。

## 部位ごとのゲインと外部メモリ

FmEngineApi の任意エクスポートのうち、部位ごとのゲインの 3 関数をエクスポート
していますが、`DSG` は部位を持ちません。OR1〜OR4 / RH1 / RH2 の端子ごとの
バランスは調整できないので、音量は `FmEngine_SetGain` で設定します。

| 関数 | 戻り値 |
|---|---|
| `FmEngine_GetPartMask` | `FM_OK` (マスクは 0) |
| `FmEngine_SetPartGain` / `FmEngine_GetPartGain` | `FM_ERR_INVALID_ARG` |

`DSG` は外部メモリを持たないので、`FmEngine_SetMemory` は `FM_ERR_UNAVAILABLE`、
`FmEngine_GetMemorySize` は 0 を返します。任意エクスポートの
`FmEngine_SetMemoryEx` はエクスポートしていません。

## チップコア単体での利用

```c
#include "ym2163.h"

YM2163 *chip = YM2163_new(1000000, 48000);   /* clock, 出力レート */

YM2163_write_reg(chip, 0x88, 0x13);          /* エンベロープ1 / SUS=1 / Cl */
YM2163_write_reg(chip, 0x8C, 0x01);          /* 0dB / OR1 端子へ */
YM2163_write_reg(chip, 0x80, 568 & 0x7F);    /* DV 下位 */
YM2163_write_reg(chip, 0x84, 0x48 | (568 >> 7));  /* KON / oct2 / DV 上位 → 440Hz */

int16_t s = YM2163_calc(chip);               /* 全端子合成 */

int16_t pins[YM2163_NUM_PINS];
YM2163_calc_pins(chip, pins);                /* OR1〜OR4, RH1, RH2 の内訳 */

YM2163_delete(chip);
```

`YM2163_write_bus()` を使えば実チップと同じバスプロトコル
(D7 = 1 でアドレスラッチ、D7 = 0 でデータ) で駆動できます。
レート変換が不要なら `YM2163_calc_native()` でネイティブレートの
1 ステップを直接取り出せます。

## 実装上の注記

データシートに数値の規定がない部分は以下のように扱っています。

- **波形メモリ** — データシートの図1 をトレースして得た 6 ビット符号付きの値です。
  読み取り誤差として ±1 程度を含む可能性があります (`core/ym2163.c` の `WAVE_ROM`)。
- **リズム音の音源** — 波形・音程・減衰時間の規定がないため、実機の音に合わせた
  近似です。BD (100Hz) と HC (260Hz) は矩形波のブリップ音、SDN は矩形波の
  ブリップ音 (300Hz) に LFSR ノイズを等分に混ぜたもの、HH は LFSR ノイズのみ。
  減衰時間は BD 250ms / HC 180ms / SDN 130ms / HH クローズ 45ms・オープン 300ms
  としています。
- **フォーシングダンプの時定数** — 規定がないため 5ms としています。
- **リズムレベル** — 線形減衰として実装しています (レベル 31 が最小音量、無音ではない)。
- **直流阻止** — Or / Pf / Hc の波形は直流成分を持ち、エンベロープがそれを振幅変調
  します。実機の出力段と同じくエンジン層で交流結合 (5Hz ハイパスフィルタ) しています。
  コア (`core/`) の出力は直流成分を含んだままです。
- **端子のミキシング** — OR1〜OR4 / RH1 / RH2 は実機では外部負荷抵抗 (標準 1kΩ) で
  ミキシングします。`FmEngine_Generate` は全端子を単純加算してモノラル出力し、
  L/R に同じ信号を出します。

## ライセンス

MIT License
