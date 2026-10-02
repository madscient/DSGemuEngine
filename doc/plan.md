# DSGemuEngine 作業計画・経緯

AI がセッションをまたいで作業を引き継ぐための文書。利用者向けの現在の仕様は
`README.md`、このリポジトリでの作業規則は `CLAUDE.md` にある。

## 確度の読み方

| 印 | 意味 |
|---|---|
| **確認済み** | 走らせて確かめた。何でどう確かめたかを添える |
| **確認済み(読解)** | ソースや文書を読んで確かめた。走らせてはいない |
| **未検証** | 作ったが動かして確かめていない |
| **推測** | 出典を示せない。根拠を一行添える |

## 1. 追随している FmEngineApi の版

| 対象 | 版 | 備考 |
|---|---|---|
| madscient/FMEngineTest `docs/FmEngineApi.md`（仕様の正） | `866f4a3` | 部位ゲイン `e39b206`、外部メモリの割り当て `e002890`、外部メモリの説明の明確化 `c0589c1`、clock=0 の廃止 `866f4a3` までを読んだ |
| madscient/YMEngine `src/FmEngineApi.h`（参照ヘッダ） | `7d8ed2d` | `src/FmEngineApi.h` はこの写しで、一字も変えていない（**確認済み**: `git hash-object` が `7d8ed2d:src/FmEngineApi.h` の blob と一致） |

ヘッダの注記と本エンジンの挙動の違い: `7d8ed2d` のヘッダは `FmEngine_SetMemory` に
ついて「data は複製せず参照する」と書き、`FmEngine_SetMemoryEx` を宣言する。どちらも
YMEngine の実装の記述で、本エンジンの `SetMemory` は常に `FM_ERR_UNAVAILABLE` を返し、
`SetMemoryEx` は定義しない（§2.3）。写しの規則に従い、ヘッダは直さない。宣言だけで
定義しない関数はエクスポートされない（**確認済み**: §2.4 の `dumpbin /exports`）。

## 2. 2026-10-02 FmEngineApi の改定への追随

変更前は `a66283a`（仕様書・ヘッダの改定前の版に準拠していた）。

### 2.1 `FmEngine_AddChip` の clock=0 を廃止

- 仕様書 `866f4a3`（利用者の決定）: `clock` が 0 なら `FM_ERR_INVALID_ARG`。エンジンは
  既定のクロックを持たない。FMEngineTest はパッチの `clock`（必須）を渡し、無いチップは
  スキップする
- 実装: `kChipTable` の既定クロックを消し、`AddChip` は `clock` が 0 なら、チップ名を
  引く前に `FM_ERR_INVALID_ARG` を返す。チップは追加せず、`out_id` にも書かない。
  検査の順は YMEngine `7d8ed2d` の `FmEngine_AddChip` と同じ（**確認済み(読解)**）
- `patches/dsg.json` に `"clock": 1000000` を足した。`_comment` に書いてあった前提の
  クロックで、変更前の既定値と同じ。今までの出力は変わらない（§2.4）
- README: 対応チップ一覧から既定クロックの列を消し、ネイティブレートを `clock / 4`
  にした。冒頭のチップ紹介の「標準クロック 1MHz」はデータシートの規定なので残した

決めたこと（本セッションで判断。利用者とはまだ確認していない）:
コア `core/ym2163.c` の `YM2163_new` / `YM2163_set_clock` は、clock=0 を
`YM2163_DEFAULT_CLOCK`（1MHz）に読み替えるまま残した。

- 理由: 仕様が縛るのは FmEngineApi で、コアは FmEngineApi に依存しない単体の API。
  エンジン層が 0 を先に弾くので、FmEngineApi からはコアの既定値に届かない
- 前提: コアが FmEngineApi から独立した API であり続けること
- 見送った案: コアからも既定クロックを除く。理由: コア API の契約
  （`YM2163_new` / `YM2163_set_clock` の 0 の扱い）が変わり、コアを単体で使う側に
  及ぶ。仕様の要求ではない
- やり直しの値段: コアの 2 か所の三項演算子とマクロ、README の「チップコア単体での
  利用」の 1 文

未決（変更前からある問題。今回は扱わない）: clock が 1〜3 Hz だとネイティブレートが
0 になる。`YM2163_set_rate` の `resample_step` が 0 になり、`YM2163_calc_pins` は
`acc[p] / chip->resample_step`（0.0 / 0.0 = NaN）を `int16_t` に変換する。C では
未定義動作（**確認済み(読解)**。走らせていない）。リズムの音程計算
（`update_rhythm_pitch`）だけは `nr` を 1 に置き換えて 0 除算を避けている。
エンジン層で弾くなら `AddChip` の戻り値（`FM_ERR_INVALID_ARG` など）を決める必要が
あり、外から見える値なので利用者に聞く。

### 2.2 部位ゲイン

- `FmEngine_SetPartGain` / `FmEngine_GetPartGain` / `FmEngine_GetPartMask` を
  エクスポートし、DSG は部位を持たないとした。`GetPartMask` は `FM_OK` で 0、
  `Set/GetPartGain` は常に `FM_ERR_INVALID_ARG`。未知の `chip_id`・null の出力
  ポインタを `FM_ERR_INVALID_ARG` にするのは EPSGemuEngine（`061cece`）に揃えた
- 根拠: 仕様書の部位の表に DSG は無く、表に無いチップは部位を持たないと仕様書にある
  （**確認済み(読解)**）
- 3 関数は任意のエクスポートで、エクスポートしなくても呼び出し側の扱いは同じ。
  エクスポートした理由: ヘッダが宣言する関数を DLL も持つようにするため。仕様が
  部位ゲインを必須に上げても準拠のままでいられる
- 前提: 仕様書の部位の表に DSG が載らないこと

未決（利用者に相談する）: DSG は OR1〜OR4（楽音。チャンネルごとに F1〜F4 で出力先を
選ぶ）・RH1（BD / HC）・RH2（SDN / HHO / HHD）を別々の端子から出す。仕様書の部位の
定義「チップが別々の端子から出す出力」に当てはまる。部位にするなら番号を仕様書で
割り当てる必要があり（`FM_PART_OPL4_DO2 = 8` の次から）、本リポジトリだけでは
決められない。

- 載せた場合の本エンジンの作業: `Generate` を `YM2163_calc_pins` の端子ごとの内訳に
  部位ゲインを掛けて足す形にする。直流阻止は今は合計 1 本に掛けているので、L/R を
  別々に掛ける（L/R でゲインが違うと合計が L/R で別の信号になる）
- 載せた場合の他リポジトリの作業: FMEngineTest の仕様書の表、YMEngine のヘッダの
  `FmPart`

### 2.3 外部メモリ（`FmEngine_SetMemoryEx`）

- エクスポートしない。`FmEngine_SetMemory` は今までどおり `FM_ERR_UNAVAILABLE`、
  `FmEngine_GetMemorySize` は 0
- 根拠: 仕様書の判断基準は「外部メモリのバスが外に出ているチップを扱うエンジンには
  エクスポートを勧める」。DSG の波形は内蔵 ROM だけで、外部メモリのバスを持たない
- 見送った案: エクスポートし、どの `mem_type` にも「チップが持たない `mem_type`」として
  `FM_ERR_INVALID_ARG` を返す。理由: シンボルが無い場合、呼び出し側は
  「`SetMemory` だけが使える」と扱うので結果は同じ。仕様書の判断基準にも当たらない
- やり直しの値段: 関数 1 つと README の 1 文

### 2.4 確認

**確認済み**（変更前 `a66283a` と変更後を、MSVC 19.51・x64・Release で別々に
ビルドした。どちらも警告なし）:

- `dumpbin /exports`: 変更前は必須 14 シンボル、変更後は必須 14 + 部位ゲイン 3 の
  17 シンボル。`FmEngine_SetMemoryEx` はどちらにも無い
- DLL を `LoadLibrary` で読む検査プログラム（15 項目）が変更後ですべて通った。
  `AddChip("DSG", 0)` が `FM_ERR_INVALID_ARG` でチップを追加せず `out_id` も
  書かない、未知のチップ名でも clock=0 は `FM_ERR_INVALID_ARG`、未知のチップ名と
  1MHz は `FM_ERR_UNKNOWN_CHIP`、1MHz と 2MHz でネイティブレートが 250,000 と
  500,000、部位ゲインの 3 関数が §2.2 のとおり（部位 0〜8）。変更前の DLL では
  clock=0 の項目と部位ゲインのシンボルの項目が落ちた
- 固定のレジスタ列（楽音 4 チャンネルとリズム 5 種、48kHz で 7.5 秒）の出力の
  ダンプが、変更前の clock=0、変更前の 1MHz、変更後の 1MHz でビット一致した。
  変更後に 1.1MHz にすると一致しない（比較がクロックの違いを見分けている）
- FMEngineTest `866f4a3` をソースからビルドし、変更後の DLL と `patches/dsg.json` を
  WAV に書き出した（15.00 秒）。clock 対応前の FMEngineTest の手元のビルド（ビルド元の
  コミットは確かめていない）・変更前の DLL・変更前のパッチで書き出した WAV と
  バイト一致した。`clock` の無い変更前のパッチを `866f4a3` で読むと `[SKIP]` になる
  （パッチの `clock` が実際に渡っている）

**未検証**: Linux / macOS（GCC / Clang、`-fvisibility=hidden`）でのビルドと
エクスポート。
