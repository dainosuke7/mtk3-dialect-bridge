#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy==2.5.3"]
# ///
"""ESC-50 のクリップを PC のスピーカーから決まった順で鳴らし、鳴らした時刻とクラスを残す。

ボードの対照試験に使う。誤報 (鳴っていないのに通知が出る) と検出漏れを、このログと
ボードの UART ログ (logs/uart.log の1行 JSON) を突き合わせて数える。

進行 (既定):
    0〜60秒    無音。この間にボードが出した通知は全部誤報
    60秒以降   5秒のクリップを15秒間隔で10本。dog / crying_baby / crackling_fire /
               sneezing / clock_tick をこの順で2巡 (通知対象4クラス + 対象外の clock_tick)

使い方 (uv が依存パッケージを用意する):
    uv run scripts/aed_play_test.py <ESC-50>
    uv run scripts/aed_play_test.py <ESC-50> --dry-run              鳴らさずに進行だけ見る
    uv run scripts/aed_play_test.py <ESC-50> --silence 5 --interval 6   短くして動作確認

出力: logs/play_<YYYYmmdd_HHMMSS>.txt
    1行 = [+62.0s] dog 5-203128-A-0.wav   (# で始まる行は条件のメモ)

注意:
    - 再生は winsound なので Windows でだけ動く。音量は OS 側で一定にしておくこと
      (このスクリプトからは音量を変えられない)
    - ボードのログとは別時計なので、突き合わせは「開始からの経過秒」で行う。
      ボードは約 0.96 秒ごとに窓を処理するので、窓の番号 x 0.96 秒が経過秒の目安になる
    - クリップは scripts/aed_clips.py の CLIPS (ESC-10、fold 5) から取る。
      メタデータの照合も aed_clips.py の read_meta / clip_path を使う
"""

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from aed_clips import CLIPS, clip_path, read_meta  # noqa: E402

try:
    import winsound
except ImportError:		# Windows 以外。--dry-run なら動く
    winsound = None

REPO = Path(__file__).resolve().parent.parent
LOG_DIR = REPO / "logs"

# 鳴らす順 (1巡5本 x REPEAT)。clock_tick は通知対象外のクラスで、誤報の見え方を見るために入れる
PLAY_ORDER = ["dog", "crying_baby", "crackling_fire", "sneezing", "clock_tick"]
REPEAT = 2
CLIP_LEN_S = 5.0		# ESC-50 のクリップは全部5秒

SILENCE_S = 60.0
INTERVAL_S = 15.0


def wait_until(t0: float, sec: float) -> None:
    """開始から sec 秒になるまで待つ。"""
    while True:
        left = t0 + sec - time.monotonic()
        if left <= 0:
            return
        time.sleep(min(left, 0.05))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("esc50", type=Path, help="ESC-50 のディレクトリ (meta/ と audio/)")
    ap.add_argument("--silence", type=float, default=SILENCE_S,
                    help=f"最初の無音の長さ [秒] (既定 {SILENCE_S})")
    ap.add_argument("--interval", type=float, default=INTERVAL_S,
                    help=f"クリップを鳴らす間隔 [秒] (既定 {INTERVAL_S})")
    ap.add_argument("--dry-run", action="store_true", help="鳴らさずに時刻とログだけ")
    args = ap.parse_args()

    if args.interval < CLIP_LEN_S:
        sys.exit(f"--interval は {CLIP_LEN_S} 秒以上にすること (クリップが重なる)")
    if winsound is None and not args.dry_run:
        sys.exit("winsound が使えない (Windows で実行するか --dry-run を付けること)")

    # 鳴らす順にクリップを決める。1巡目は各クラスの1本目、2巡目は2本目
    meta = read_meta(args.esc50)
    plan = []
    for rep in range(REPEAT):
        for label in PLAY_ORDER:
            if rep >= len(CLIPS[label]):
                sys.exit(f"{label}: CLIPS に {rep + 1} 本目が無い")
            fn = CLIPS[label][rep]
            plan.append((label, fn, clip_path(meta, fn, label, args.esc50)))

    LOG_DIR.mkdir(exist_ok=True)
    log_path = LOG_DIR / f"play_{datetime.now():%Y%m%d_%H%M%S}.txt"
    total = args.silence + args.interval * (len(plan) - 1) + CLIP_LEN_S

    with open(log_path, "w", encoding="utf-8", newline="\n") as log:

        def out(line: str) -> None:
            print(line)
            log.write(line + "\n")
            log.flush()		# 途中で止めても残るように毎行書き出す

        out(f"# aed_play_test start {datetime.now():%Y-%m-%d %H:%M:%S}"
            f"{' (dry run: 音は鳴らさない)' if args.dry_run else ''}")
        out(f"# silence {args.silence:.1f}s -> {len(plan)} clips x {CLIP_LEN_S:.0f}s"
            f" every {args.interval:.1f}s, total {total:.1f}s")
        out(f"# clips from {args.esc50} (ESC-10, fold 5)")
        out("# 0s から silence の間の通知は誤報。時刻は開始からの経過秒 (ボードとは別時計)")

        t0 = time.monotonic()
        for i, (label, fn, path) in enumerate(plan):
            wait_until(t0, args.silence + args.interval * i)
            out(f"[+{time.monotonic() - t0:.1f}s] {label} {fn}")
            if args.dry_run:
                continue
            try:
                # 同期再生。5秒ブロックしてから次の待ちに入る
                winsound.PlaySound(str(path), winsound.SND_FILENAME | winsound.SND_NODEFAULT)
            except RuntimeError as e:
                out(f"# [+{time.monotonic() - t0:.1f}s] PlaySound failed: {e}")

        wait_until(t0, total)
        out(f"# aed_play_test end (+{time.monotonic() - t0:.1f}s)")

    print(f"\nwrote {log_path.relative_to(REPO)}")


if __name__ == "__main__":
    main()
