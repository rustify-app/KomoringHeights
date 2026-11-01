# 詰将棋モード

生成作品の完全性と審美性を分離する方針、追い詰めの拒否条件、今後の構想生成については [詰将棋生成の審美方針](tsume-aesthetic-policy.md) を参照してください。

既存の `go mate` / `user tsume_solve` は高速な df-pn 求解モードとして維持されています。作品としての完全性を検査するときは、全幅 AND/OR 探索を行う次のコマンドを使います。

```text
position sfen <SFEN>
user tsume_verify <上限手数> <上限ノード数> [既知のUSI手順...]
```

攻方は先手で、攻方ノードでは合法な王手だけ、玉方ノードでは無駄合を除く全合法回避手を探索します。上限ノード数に達した検査は `complete:false` となり、正式な完成作品にはなりません。SFENで省略された玉方の残り持駒は、単玉39枚または双玉40枚の完全駒集合との差分から復元され、完全なSFENとして探索されます。

結果は `info string tsume_json {...}` として出力されます。詰み手数、USI手順、人間向け手順、同手数変化、余詰、早詰、千日手、駒余り、不要駒、探索ノード数、技巧、11種の評価スコアと判定理由を含みます。

## 完全作解答モード（df-pn ベース）

全幅探索の `tsume_verify` は 15 手を超えると現実的な時間で終わらないため、詰パラ級の作品を解くときは次のコマンドを使います。

```text
position sfen <SFEN>
user tsume_answer [部分問題1つあたりms=3000] [全体df-pn ms=30000] [作意USI手順...]
```

KomoringHeights の df-pn は無駄合も有効な応手として数えるので、その手数は詰将棋ルールでの手数の**上界**にしかなりません。`tsume_answer` は df-pn を部分問題の判定器として使い、次の規則で詰将棋ルールの解を求めます。

- **無駄合**: 玉方の打ち合（王手している飛び駒と玉の間への打）は、取った後に取った駒を持駒から除いても詰むなら応手から外す（移動合と王手駒を取る手は外さない）
- **攻方ノード**: 合駒を禁止した df-pn の手数（**下界**）の小さい王手から調べ、最短を求める
- **玉方ノード**: 合駒ありの df-pn の手数（上界）の大きい応手から調べ、無駄合を除いた最長を求める。同手数なら駒が余らない手順を選ぶ
- **解答木**（JSON の `tree`）: 解答を1本の手順でなく木で返す。攻方手番では残り手数以内に詰む王手を全て、玉方手番では無駄合を除く最長の応手（駒余りにならない応手があればそれだけ）を展開する。同じ升への合で続きが同じものは最も安い駒の1つにまとめ、他の駒は `also` に並べる。各手番で `answer` の手を先頭に置く。`user tsume_answer_tree <最大ノード数> [ms]` で予算を決め（既定 300 ノード、ms 省略時は全体 df-pn ms）、超えた分は展開せず `truncated: true` / `cut: true` とする。`answer` の手順は予算に関係なく展開する。0 ノードで木を出力しない。時間内に詰むか判定できなかった手は `unknown: true`（`mate: -1`）で残し、その数を `unresolved` に出す（複数スレッドでは短い df-pn が判定できずに終わることがあるので、木を使うなら Threads 1 を勧める）
- **余詰**: 作意（合法でなければ求めた解）の最終手以外の攻方手番で、他の王手が残り手数以内に詰むかを調べる。合駒なしでも詰まなければ詰まないと判定し、詰めばルール上の手数を求める

df-pn の呼び出しは全て手数上限付きで行い、時間切れで判定できなかった部分問題は `unknown` として報告します（`info string [tsume_answer] unresolved ...`）。結果は `info string tsume_answer {...}` の JSON で、`rawMatePly`（df-pn 単体の手数）、`matePly` / `usi`（ルール上の解）、`intendedLegal` / `sameAsIntended`、`alternatives`（余詰・早詰・未解決）、`pieceSurplus`、`perfect` を含みます。残り手数より長い別詰（迂回を含む）は調べません。

KIF（局面図+作意）をまとめて検査するには:

```text
python3 tools/tsume_answer_batch.py source/KomoringHeights-by-gcc *.kif --node-ms 3000 --raw-ms 30000 --jsonl out.jsonl
```

## 保存と再検証

```text
user tsume_save <上限手数> <上限ノード数> <JSONLファイル> [既知のUSI手順...]
user tsume_load <JSONLファイル> [上限手数] [上限ノード数]
```

`tsume_load` は保存SFENを再び完全探索するため、保存時の結果を再現検査できます。SFEN単体はJSONの `sfen` / `normalizedSfen` から再出力できます。

## 生成と手数延長

```text
user tsume_generate <7以上の奇数手数> [件数] [SFENファイル]
user tsume_batch_generate <開始手数> <終了手数> <step> [各件数] [SFENファイル]
user tsume_extend <件数> <上限手数> <上限ノード数> <JSONLファイル>
```

生成器は候補を df-pn で絞った後、全幅検証、一意性、駒余り、不要駒、正規化後の再検証を通過した作品だけを保存します。SFENファイルと同時に `<ファイル名>.jsonl` が保存されます。`tsume_extend` は現在局面に駒の追加・削除・移動・駒種変更・成不成変更・玉移動を加え、複数の完全作候補を保存します。各レコードには親ID、差分、変更前後の手数、類似度と延長理由が含まれます。

正規化では上移動、右移動、左右反転の各候補を再び完全探索し、合法手・詰み手数・変化構造が同型の場合だけ変換します。同型作品には正規化SFEN由来の共通 `canonicalId` が付与されます。

## 回帰テスト

Windowsビルド後は次のテストで、USI通信、完全駒集合復元、保存再読込、平行移動のcanonical ID、無効局面の安全な拒否、既存解答モードを一括確認できます。

```powershell
.\script\test_tsume_mode.ps1 -Engine .\source\KomoringHeights-by-gcc.exe
```
