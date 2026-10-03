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
| madscient/FMEngineTest `docs/FmEngineApi.md`（仕様の正） | `ca502c5` | 必須 11 シンボル。部位と外部メモリは名前で指定する（§3） |
| madscient/FMEngineTest `include/FmEngineApi.h`（ヘッダの正本） | `ca502c5` | `src/FmEngineApi.h` はこの写しで、一字も変えていない（**確認済み**: `git hash-object` が `ca502c5:include/FmEngineApi.h` の blob と一致）。ヘッダが最後に変わったのは `831947d` |
| madscient/FMEngineTest `docs/CHANGELOG.md`（各エンジンに求める対応） | `ca502c5` | 「FmEngine_GetNativeRate を廃止する」「外部メモリを名前で指定する」「部位を名前で指定する／ヘッダの正本をこのリポジトリに置く」の 3 節を読んだ |

ヘッダは外部メモリの 4 関数も宣言するが、本エンジンは定義しない（§3.3）。宣言だけで
定義しない関数はエクスポートされない（**確認済み**: §3.4 の `dumpbin /exports`）。

§2 は 2026-10-02 時点の仕様への追随の記録で、写し元は madscient/YMEngine
`src/FmEngineApi.h` の `7d8ed2d` だった。

## 2. 2026-10-02 FmEngineApi の改定への追随

変更前は `a66283a`（仕様書・ヘッダの改定前の版に準拠していた）。
§2.2 と §2.3 の決定は §3 で改めた。

### 2.1 `FmEngine_AddChip` の clock=0 を廃止

- 仕様書 `a17c372`（利用者の決定）: `clock` が 0 なら `FM_ERR_INVALID_ARG`。エンジンは
  既定のクロックを持たない。FMEngineTest はパッチの `clock`（必須）を渡し、無いチップは
  スキップする
- 実装: `kChipTable` の既定クロックを消し、`AddChip` は `clock` が 0 なら、チップ名を
  引く前に `FM_ERR_INVALID_ARG` を返す。チップは追加せず、`out_id` にも書かない。
  検査の順は YMEngine `7d8ed2d` の `FmEngine_AddChip` と同じ（**確認済み(読解)**）
- `patches/dsg.json` に `"clock": 1000000` を足した。`_comment` に書いてあった前提の
  クロックで、変更前の既定値と同じ。今までの出力は変わらない（§2.4）
- README: 対応チップ一覧から既定クロックの列を消し、ネイティブレートを `clock / 4`
  にした。冒頭のチップ紹介の「標準クロック 1MHz」はデータシートの規定なので残した

決めたこと（AI の判断。2026-10-02 に利用者へ報告し、変更の指示は受けていない）:
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

未決（変更前からある問題。扱っていない）: clock が 1〜3 Hz だとネイティブレートが
0 になる。`YM2163_set_rate` の `resample_step` が 0 になり、`YM2163_calc_pins` は
`acc[p] / chip->resample_step`（0.0 / 0.0 = NaN）を `int16_t` に変換する。C では
未定義動作（**確認済み(読解)**。走らせていない）。リズムの音程計算
（`update_rhythm_pitch`）だけは `nr` を 1 に置き換えて 0 除算を避けている。
エンジン層で弾くなら `AddChip` の戻り値（`FM_ERR_INVALID_ARG` など）を決める必要が
あり、外から見える値なので利用者に聞く。

### 2.2 部位ゲイン（番号で指定する形。§3.2 で改めた）

- `FmEngine_SetPartGain` / `FmEngine_GetPartGain` / `FmEngine_GetPartMask` を
  エクスポートし、DSG は部位を持たないとした。`GetPartMask` は `FM_OK` で 0、
  `Set/GetPartGain` は常に `FM_ERR_INVALID_ARG`。未知の `chip_id`・null の出力
  ポインタを `FM_ERR_INVALID_ARG` にするのは EPSGemuEngine（`061cece`）に揃えた
- 根拠: 当時の仕様書の部位の表に DSG は無く、表に無いチップは部位を持たないと
  仕様書にあった。部位の番号は仕様書で割り当てるもので、本リポジトリだけでは
  足せなかった

### 2.3 外部メモリ（§3.3 で改めた）

- `FmEngine_SetMemoryEx` はエクスポートせず、当時は必須だった `FmEngine_SetMemory` は
  `FM_ERR_UNAVAILABLE`、`FmEngine_GetMemorySize` は 0 を返した
- 根拠: DSG の波形は内蔵 ROM だけで、外部メモリのバスを持たない

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
- FMEngineTest `a17c372` をソースからビルドし、変更後の DLL と `patches/dsg.json` を
  WAV に書き出した（15.00 秒）。clock 対応前の FMEngineTest の手元のビルド（ビルド元の
  コミットは確かめていない）・変更前の DLL・変更前のパッチで書き出した WAV と
  バイト一致した。`clock` の無い変更前のパッチを `a17c372` で読むと `[SKIP]` になる
  （パッチの `clock` が実際に渡っている）

**未検証**: Linux / macOS（GCC / Clang、`-fvisibility=hidden`）でのビルドと
エクスポート。

## 3. 2026-10-03 FmEngineApi の改定への追随（名前での指定、GetNativeRate の廃止）

変更前は `e0a250a`。

### 3.1 仕様の変更点と、本エンジンに求められた対応

仕様書・ヘッダ・CHANGELOG の `ca502c5` を読んだ（**確認済み(読解)**）。

- 必須シンボルは 14 個から 11 個。`FmEngine_GetNativeRate` と
  `FmEngine_GetMemorySize` は廃止、`FmEngine_SetMemory` は任意の組に移った
- 部位は名前の文字列で指定する。`FmPart` と `FmEngine_GetPartMask` は無くなり、
  `FmEngine_GetPartCount` / `FmEngine_GetPartName` / `FmEngine_SetPartGain` /
  `FmEngine_GetPartGain` の 4 つを組でエクスポートする。呼び出し側は
  `FmEngine_GetPartCount` の有無で判定する
- 外部メモリも名前で指定する。`FmEngine_GetMemoryCount` / `FmEngine_GetMemoryName` /
  `FmEngine_SetMemory` の 3 つが任意の組で、`FmEngine_SetMemoryEx` はその上に足す
- 仕様書の表に無いチップの部位・外部メモリは、名前と既定値をエンジンが決める
  （ASCII の英大文字・数字・`_`）。仕様書とヘッダの変更は要らない
- ヘッダの正本は FMEngineTest の `include/FmEngineApi.h` になった
- CHANGELOG が本エンジンに求める対応: ヘッダを写し直す。`FmEngine_GetNativeRate` の
  エクスポートをやめる。外部メモリの関数のエクスポートをやめる。部位は「0 を返す
  スタブにするか、4 関数のエクスポートをやめる（どちらも準拠）」

変更前の DLL は、`FmEngine_GetPartCount` を持たずに番号指定の
`FmEngine_SetPartGain` / `FmEngine_GetPartGain` をエクスポートしており、新しい
仕様とは互換性が無い形だった。

### 3.2 出力端子を部位にする

決定（利用者。2026-10-03）: DSG の 6 つの出力端子を部位にする。名前は `OR1` / `OR2` /
`OR3` / `OR4` / `RH1` / `RH2`（データシートの端子名）、既定値はすべて 1.0。
利用者に示した条件: 既定値のままなら、出力は変更前とビット一致する。

- 前提: 仕様書の表に DSG が載っていないこと。載ったら表の名前と既定値に合わせる
- やり直しの値段: 名前を変えると、`kPartNames`、README、それに名前を使い始めた
  アプリケーションと設定ファイルに及ぶ。部位を足すだけなら互換は壊れない
- 見送った案: 部位を持たず、4 関数をエクスポートしない（EPSGemuEngine と同じ形）。
  理由: 利用者が 6 端子を部位にすることを選んだ。§2.2 で未決にしていた「端子を部位に
  するか」は、これで決着した

実装（`src/DSGemuEngine.cpp`）:

- 部位の添字はコアの `YM2163_PIN_*` と共通。並びが食い違うと `static_assert` で
  ビルドが止まる
- `chipCalcStereo` は `YM2163_calc_pins` の端子ごとの値に部位ゲインを掛けて L/R 別に
  加算し、16 ビットの範囲で飽和させてから、L/R それぞれに直流阻止を掛ける
- 飽和はコアの `YM2163_calc` が全端子の合成に掛けていたものと同じ。入れないと、
  1 つのチャンネルを複数の端子へ出して合成が 16 ビットを超える場合に、既定値でも
  出力が変更前と変わる（§3.4 の対照で確かめた）
- `FmEngine_GetPartName` は端子の並び（`OR1`〜`OR4`、`RH1`、`RH2`）で返す。仕様は
  並びを定めておらず、README にも並びは約束していない
- `FmEngine_GetPartGain` の出力ポインタは片方ずつ NULL でよい（`FmEngine_GetGain` と
  同じ扱い）。仕様は NULL の出力ポインタを定めていない
- 部位ゲインの読み書きは排他していない。`FmEngine_SetGain` と同じ作りに揃えた。
  `Generate` と並行に書くと、厳密には `float` へのデータ競合になる（変更前からの
  `SetGain` も同じ）

見送った案:

- 直流阻止を端子ごと（6 本）に掛けてから部位ゲインを掛ける。部位ゲインを発音中に
  変えても直流のステップが出ない利点がある。理由: 浮動小数の加算の順が変わり、
  既定値での出力が変更前とビット一致しなくなる。やり直しの値段は `chipCalcStereo` の
  中だけだが、出力が丸め誤差の範囲で変わる
- 合成後の飽和を外す。理由: 上のとおり、既定値でも出力が変わる場合がある

**未検証**: 発音中に部位ゲインを変えたときの聞こえ方。直流成分を持つ波形
（Or / Pf / Hc）の発音中に変えると、直流のステップが 5Hz のフィルタを通る
（**推測**: 実装の構造からの判断。鳴らしていない）。

### 3.3 やめたエクスポート

- `FmEngine_GetNativeRate`: 定義を消し、`ChipEntry` の `native_rate` も消した。
  コアの `YM2163_native_rate` はコア API として残る
- `FmEngine_SetMemory` / `FmEngine_GetMemorySize`: 定義を消した。DSG は外部メモリを
  持たないので、外部メモリの 4 関数はどれもエクスポートしない
- `FmEngine_GetPartMask`: 定義を消した
- 見送った案: `FmEngine_GetNativeRate` を残す（仕様書は一覧に無いシンボルの
  エクスポートを禁じていない）。理由: CHANGELOG が各エンジンにやめることを求めて
  おり、ヘッダの写しにも宣言が無い
- 互換: `FmEngine_GetNativeRate` を必須として読む FMEngineTest は、変更後の DLL を
  ロードできない。FMEngineTest の CHANGELOG によれば `831947d` より前の版がこれに
  あたる。走らせて確かめたのは `a17c372` だけ（§3.4）。番号指定の形でビルドした
  呼び出し側とは、部位ゲインを組み合わせられない（文字列のポインタとして番号を読む）

### 3.4 確認

**確認済み**（変更前 `e0a250a` と変更後を、MSVC 19.51・x64・Release で別々に
ビルドした。どちらも警告なし）:

- `dumpbin /exports`: 変更前は 17 シンボル、変更後は必須 11 + 部位 4 の 15 シンボル。
  変更後に `FmEngine_GetNativeRate`・`FmEngine_GetMemorySize`・`FmEngine_GetPartMask`・
  外部メモリの 4 関数は無い
- DLL を `LoadLibrary` で読む検査プログラムの API の検査（24 項目）が変更後で
  すべて通った。部位は 6 個で、名前が `OR1`〜`RH2`、範囲外と未知の `chip_id` は
  NULL / 0、既定値がすべて (1.0, 1.0)、部位ごとに設定した値が読み戻せて他の部位と
  他のチップに及ばない、未知の名前（`FM`、`or1`、空文字列、`OR5` など）・NULL・
  未知の `chip_id` は `FM_ERR_INVALID_ARG`。変更前の DLL ではシンボルの 5 項目が落ちた
- 端子の対応の検査（19 項目）が変更後ですべて通った。楽音を F1〜F4 のどれか 1 つに
  出したもの 4 通りと、リズム 5 種（BD / HC / SDN / HHO / HHD）について、6 部位を
  1 つずつ 0 にして出力のピークを取った。該当する部位（F1〜F4 は `OR1`〜`OR4`、
  BD / HC は `RH1`、SDN / HHO / HHD は `RH2`）を 0 にしたときだけ出力が厳密に 0 に
  なり、ほかの部位を 0 にしてもピークは変わらない。該当する部位を (1, 0) にすると、
  L は変わらず R が厳密に 0 になる
- 既定値での出力が変更前とビット一致した（48kHz・7.5 秒のダンプ）。レジスタ列は
  2 通り: §2.4 と同じ列、楽音 4 チャンネルを同じ音程で 4 端子すべてに出して合成が
  16 ビットを超える列（`FmEngine_SetGain` は 0.2）
- 対照: 合成後の飽和を外した変種の DLL は、1 つ目の列では変更前と一致し、2 つ目の
  列では 360,000 サンプル中 232,538 サンプルが食い違う（差の最大は 0.298）。
  2 つ目の列が飽和の経路を通っていて、飽和が無いと一致しない
- 全部位を (0.5, 0.5) にした出力が、`FmEngine_SetGain` を (0.5, 0.5) にした出力と
  ビット一致した（部位ゲインと `SetGain` が掛け合わさる）。`OR1` を 0 にした出力は
  既定値の出力と一致しない
- FMEngineTest `ca502c5` をソースからビルドし、`patches/dsg.json` を WAV に書き出した
  （15.00 秒）。変更前の DLL と変更後の DLL でバイト一致した。§2.4 の WAV とも一致する
- `a17c372` のソースからビルドした FMEngineTest は、変更後の DLL を
  `FmEngine_GetNativeRate not found in DLL` でロードできない

FMEngineTest は部位ゲインを呼ばない（**確認済み(読解)**: FMEngineTest の
`CLAUDE.md`）。部位の関数を呼んだのは、上の検査プログラムだけ。検査プログラムは
リポジトリに入れていない。

**未検証**: Linux / macOS（GCC / Clang、`-fvisibility=hidden`）でのビルドと
エクスポート。実際のアプリケーションからの部位ゲインの呼び出し。

## 4. 未決の一覧

- コアの既定クロック（clock=0 を 1MHz に読み替える）を残したままでよいか（§2.1）
- clock が 1〜3 Hz のときの未定義動作（§2.1）
- 発音中に部位ゲインを変えたときの直流のステップ（§3.2）
- GCC / Clang でのビルド（§3.4）
