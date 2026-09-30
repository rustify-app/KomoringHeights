# 検証と美的評価

生成された局面を「完全な詰将棋作品」として認定するために、**完全性検証** と **美的評価** の二段構えで判定します。

## 完全性検証: ExhaustiveVerifier

df-pn は「詰むか / 詰まないか」を高速に判定しますが、**別解の有無**や**変同の数**は探索しません。そこで独自の `ExhaustiveVerifier`（`tsume_workbench.hpp:259-420`）で**有界 AND/OR 全探索**を行います。

### 探索構造

- **OR ノード（攻方の手番）**: 最短の詰み手数を求める
- **AND ノード（受方の手番）**: 最長の詰み手数を求める + 別解を数える
- **キャッシュ**: `(board_hash, remaining_ply)` → 評価結果 のメモ化

### 判定結果（`VerifyResult` 構造体）

| フィールド | 意味 |
|---|---|
| `proof` | `kMate` / `kNoMate` / `kRepetition` / `kUnknown` |
| `mate_ply` | 詰みまでの手数（`kMate` 時） |
| `unique` | 攻方の初手が唯一手 |
| `alternative_mate` | 複数の初手で詰む（余詰） |
| `shorter_mate` | 想定より短い詰みが存在 |
| `hendou_count` | 同一手数の別解の総数（変同数） |
| `futile_interposition` | 玉と王手駒の間への打ち合で、取って1手で詰むものを除外（移動合・王手駒を取る手は除外しない） |
| `line_alternative` / `line_alternatives` | 作意途中（最終手を除く）の余詰 |
| `principal` | 最短の主要変化 |
| `longest_lines` | 最長の受方応手を全列挙 |

### 完全作の条件

以下を全て満たすとき、この作品は **完全** です。

- `proof == kMate`
- `unique == true`（攻方初手唯一）
- `alternative_mate == false`（余詰なし）
- `line_alternative == false`（作意途中の攻方手番に別の詰み手なし。最終手は除く）
- `shorter_mate == false`（短手数解なし）
- 39/40 枚制を満たす（駒余りなし）

---

## 美的評価: AestheticScores

完全性を満たした作品に対して、**11 次元の美的スコア**を計算します（`tsume_workbench.hpp:79-89`）。

| スコア | 意味 |
|---|---|
| `legalityScore` | 全ての手が合法（0-100） |
| `uniquenessScore` | 単一解 |
| `lengthScore` | 手数が長いほど高い |
| `complexityScore` | 変化の豊かさ（受方の代替手、玉以外の受け、開き王手など） |
| `sacrificeScore` | 駒の取り・打（特に飛角） |
| `techniqueScore` | 連続捨駒・両王手・不成など技巧の総合 |
| `economyScore` | 攻方駒の稼働率（X9） |
| `visualScore` | 玉の端距離・開き（X4, X12） |
| `originalityScore` | 独創性（暫定 50、外部 DB 未接続） |
| `aestheticScore` | 上記の総合（**35 以上で `aestheticPass`**） |
| `totalScore` | 最終ランキング指標 |

### 技巧サマリ（`TechniqueSummary`）

- 攻方手数 / 受方手数 / 玉移動数
- 捨駒（大駒・連続）
- 打（打歩・打銀など）
- 成 / 不成
- 接触王手 / 開き王手 / 両王手
- 攻方の駒取り (X3)、玉の開き (X4)、駒稼働率 (X9)、玉端距離 (X12)

### 追い詰めペナルティ

**追い詰め**（chasing mate）は美的にペナルティ対象です。指標:

```
chaseRatio = (攻方の接触王手率 + 受方の玉移動率) / 2
```

以下のいずれかで**強いペナルティ**（`aestheticScore ≤ 20`, `totalScore ≤ 25`）:

- 攻方が 3 手以上あり、かつ**捨駒・開き王手・両王手のいずれもなく**、`chaseRatio ≥ 0.75`
- 玉移動率 90% 以上

これは「駒を渡さず玉を追い回すだけ」の詰将棋を美的に排除するための判定です。「禁止禁止」原則の例外に近いが、**明示的なスコアペナルティ**として実装されており、完全禁止ではありません（極端な追い詰めでも `totalScore` が 25 まで許容される）。

---

## 二段ゲート

生成された全ての局面は、以下の順で判定されます。

```
[生成局面]
    │
    ▼
完全性ゲート (ExhaustiveVerifier)
    ├── NG: 破棄
    └── OK
        │
        ▼
    39/40 枚制補完 (CompleteDefenderReserve)
        │
        ▼
    美的評価 (AestheticScores)
        │
        ▼
    aestheticPass?
        ├── OK → SFEN + JSONL に保存
        └── NG → JSONL のみ保存（aestheticReasons 付き）
```

`aestheticPass == false` の局面も JSONL には残り、**なぜ落ちたか**が `aestheticReasons` に記録されます。データセット分析時にフィルタ条件として使えます。

---

## 未実装項目

`docs/tsume-aesthetic-policy.md` に記載の通り、以下は未実装:

- 限定打（限定線）パターンの検出
- 中合の中間駒認識
- 打診手筋
- 序・急所・詰み上がりの物語構造分析
- 学習ベースのランキングモデル（現状は Hirose 変数のみ）
- 外部 DB との類似度検出（内部の `canonicalId` は座標変換不変な重複検出のみ）
