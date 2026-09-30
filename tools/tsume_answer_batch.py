#!/usr/bin/env python3
"""詰将棋KIF（局面図+作意）を KomoringHeights の `user tsume_answer` で一括解答・検証する。

usage: tsume_answer_batch.py ENGINE KIF... [--node-ms N] [--raw-ms N] [--hash MB] [--jobs N] [--threads N] [--jsonl OUT]
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

KANJI_NUM = {c: i for i, c in enumerate("〇一二三四五六七八九十", 0)}
ZEN_NUM = {c: i + 1 for i, c in enumerate("１２３４５６７８９")}
PIECES = {"歩": "P", "香": "L", "桂": "N", "銀": "S", "金": "G", "角": "B", "飛": "R", "玉": "K", "王": "K",
          "と": "+P", "杏": "+L", "圭": "+N", "全": "+S", "馬": "+B", "龍": "+R", "竜": "+R",
          "成香": "+L", "成桂": "+N", "成銀": "+S"}


def kanji_count(s):
    if not s:
        return 1
    if s.startswith("十"):
        return 10 + (KANJI_NUM[s[1]] if len(s) > 1 else 0)
    return KANJI_NUM[s[0]]


def parse_hand(text, upper):
    out = ""
    for m in re.finditer(r"([歩香桂銀金角飛])([一二三四五六七八九十]*)", text):
        n = kanji_count(m.group(2))
        c = PIECES[m.group(1)]
        out += (str(n) if n > 1 else "") + (c if upper else c.lower())
    return out


def parse_kif(path):
    raw = Path(path).read_bytes()
    for enc in ("utf-8", "cp932"):
        try:
            text = raw.decode(enc)
            break
        except UnicodeDecodeError:
            continue
    board, black_hand, white_hand, moves = [], "", "", []
    side = "b"
    prev_to = None
    for line in text.splitlines():
        line = line.rstrip()
        if line.startswith("変化"):
            break
        if line.startswith("後手の持駒") or line.startswith("上手の持駒"):
            white_hand = parse_hand(line.split("：", 1)[1], False)
        elif line.startswith("先手の持駒") or line.startswith("下手の持駒"):
            black_hand = parse_hand(line.split("：", 1)[1], True)
        elif line.startswith("後手番"):
            side = "w"
        elif line.startswith("|"):
            cells = line[1:line.rindex("|")]
            row, i = [], 0
            while i + 1 < len(cells) + 1 and i < len(cells):
                owner, piece = cells[i], cells[i + 1]
                i += 2
                if piece == "・":
                    row.append(None)
                else:
                    c = PIECES[piece]
                    row.append(c.lower() if owner == "v" else c)
            board.append(row)
        else:
            m = re.match(r" *([0-9]+) +(.+?)(?: +\( *[0-9]+:.*)?$", line)
            if not m or not board:
                continue
            mv = m.group(2).strip().rstrip("+")
            if mv.startswith(("投了", "詰", "中断", "不詰")):
                break
            if mv.startswith("同"):
                to = prev_to
                rest = mv[1:].lstrip("　 ")
            else:
                to = f"{ZEN_NUM[mv[0]]}{chr(ord('a') + KANJI_NUM[mv[1]] - 1)}"
                rest = mv[2:]
            prev_to = to
            if "打" in rest:
                moves.append(f"{PIECES[rest[0]]}*{to}")
                continue
            frm = re.search(r"\((\d)(\d)\)", rest)
            usi = f"{frm.group(1)}{chr(ord('a') + int(frm.group(2)) - 1)}{to}"
            head = rest[:rest.index("(")]
            if head.endswith("成") and not head.endswith("不成"):
                usi += "+"
            moves.append(usi)
    rows = []
    for row in board:
        s, empty = "", 0
        for c in row:
            if c is None:
                empty += 1
                continue
            if empty:
                s += str(empty)
                empty = 0
            s += c
        if empty:
            s += str(empty)
        rows.append(s)
    hand = (black_hand + white_hand) or "-"
    return f"{'/'.join(rows)} {side} {hand} 1", moves


class Engine:
    def __init__(self, path, hash_mb, threads, alternatives=True):
        self.proc = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                     cwd=str(Path(path).resolve().parent))
        self.send("usi")
        self.wait_for("usiok")
        self.send(f"setoption name USI_Hash value {hash_mb}")
        if threads:
            self.send(f"setoption name Threads value {threads}")
        self.send("isready")
        self.wait_for("readyok")
        if not alternatives:
            self.send("user tsume_answer_alternatives off")

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def wait_for(self, prefix):
        for line in self.proc.stdout:
            if line.startswith(prefix):
                return line
        raise RuntimeError("engine exited")

    def answer(self, kif, node_ms, raw_ms):
        sfen, intended = parse_kif(kif)
        self.send(f"position sfen {sfen}")
        self.send(f"user tsume_answer {node_ms} {raw_ms} {' '.join(intended)}")
        line = self.wait_for("info string tsume_answer ")
        rec = json.loads(line.split("tsume_answer ", 1)[1])
        rec["file"] = kif
        rec["intended"] = " ".join(intended)
        return rec

    def close(self):
        self.send("quit")
        self.proc.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("engine")
    ap.add_argument("kifs", nargs="+")
    ap.add_argument("--node-ms", type=int, default=3000)
    ap.add_argument("--raw-ms", type=int, default=30000)
    ap.add_argument("--hash", type=int, default=64)
    ap.add_argument("--jobs", type=int, default=1, help="並列に動かすエンジン数")
    ap.add_argument("--threads", type=int, default=0, help="エンジン1つあたりのスレッド数（0 は既定値）")
    ap.add_argument("--no-alternatives", action="store_true", help="余詰検証を省き解答だけを求める")
    ap.add_argument("--jsonl")
    args = ap.parse_args()

    import queue
    import threading
    todo = queue.Queue()
    for kif in args.kifs:
        todo.put(kif)
    lock = threading.Lock()
    out = open(args.jsonl, "a") if args.jsonl else None

    def worker():
        eng = Engine(args.engine, args.hash, args.threads, not args.no_alternatives)
        while True:
            try:
                kif = todo.get_nowait()
            except queue.Empty:
                break
            rec = eng.answer(kif, args.node_ms, args.raw_ms)
            n = len(rec["intended"].split())
            verdict = "PERFECT" if rec.get("perfect") else "NG"
            alts = [f"{a['ply']}:{a['move']}({a['kind']},{a['matePly']})" for a in rec.get("alternatives", [])]
            hen = f"変同{len(rec.get('hendou', []))}" if rec.get("hendou") else ""
            surplus = "駒余り" if rec.get("answerSurplus") else ""
            with lock:
                print(f"{kif}\t{rec.get('status')}\t{rec.get('answerPly', rec.get('matePly'))}手(作意{n}手)\t"
                      f"same={rec.get('sameAsIntended')}\t{verdict}\t{hen}{surplus}\t{rec.get('timeMs')}ms\t{' '.join(alts)}", flush=True)
                if out:
                    out.write(json.dumps(rec, ensure_ascii=False) + "\n")
                    out.flush()
        eng.close()

    threads = [threading.Thread(target=worker) for _ in range(max(1, args.jobs))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


if __name__ == "__main__":
    main()
