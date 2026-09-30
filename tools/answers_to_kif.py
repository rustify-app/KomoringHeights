#!/usr/bin/env python3
"""tsume_answer の JSONL から、解答手順を本手順にした KIF を書き出す（convert.py 用）。

usage: answers_to_kif.py RESULTS.jsonl OUT_DIR [--prefix 202609/]
元の KIF の局面図（「手数----」より前）をそのまま使い、指し手だけをエンジンの解に置き換える。
"""
import argparse
import json
from pathlib import Path

import cshogi
import cshogi.KIF as K


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("jsonl")
    ap.add_argument("out_dir")
    ap.add_argument("--prefix", default="")
    args = ap.parse_args()
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    latest = {}
    for line in open(args.jsonl, encoding="utf-8"):
        rec = json.loads(line)
        # 詰みが求まった結果のうち最新のものを使う（後の実行が時間切れでも前の解を残す）
        if rec["file"].startswith(args.prefix) and rec.get("status") == "mate" and (rec.get("answer") or rec.get("usi")):
            latest[rec["file"]] = rec
    for path, rec in sorted(latest.items()):
        moves = (rec.get("answer") or rec.get("usi") or "").split()
        if rec.get("status") != "mate" or not moves:
            print(f"skip {path}: {rec.get('status')}")
            continue
        raw = Path(path).read_bytes().decode("cp932")
        header = raw.split("手数----")[0]
        board = cshogi.Board(rec["sfen"])
        lines = [header.rstrip("\r\n"), "手数----指手---------消費時間--"]
        prev = None
        for i, usi in enumerate(moves):
            m = board.move_from_usi(usi)
            text = K.move_to_kif(m, prev)
            lines.append(f"{i + 1:>4} {text:<12}( 0:00/00:00:00)")
            board.push(m)
            prev = m
        lines.append(f"まで{len(moves)}手で詰み")
        (out / Path(path).name).write_bytes(("\r\n".join(lines) + "\r\n").encode("cp932"))
        print(f"wrote {out / Path(path).name}")


if __name__ == "__main__":
    main()
