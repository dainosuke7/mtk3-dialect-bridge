#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["onnxruntime==1.30.0", "onnx==1.23.0", "numpy==2.5.3"]
# ///
"""AED モデルを ONNX Runtime で実行し、NPU との比較用の固定入力と期待値を作る。

Phase 1 タスク5 で用意し、タスク6 でボード (NPU) の出力と比べる。

使い方 (uv が依存パッケージを用意する):
    uv run scripts/aed_ref.py <yamnet_1024_64x96_tl_qdq_int8.onnx>

モデルは STM32N6-GettingStarted-Audio v2.3.0 の
Projects/X-CUBE-AI/models/yamnet_1024_64x96_tl_qdq_int8.onnx (README 参照)。

入力の扱い:
    ONNX の入力 input_1 は float32 1x64x96x1 で、先頭の Transpose の直後に
    QuantizeLinear (scale=0.0305305421352386, zero_point=33, int8) がある。
    ボードの NPU 版 (stai_network.h) は同じ scale / zero_point の int8 を直接受け取る。
    そこで int8 の値 q を決め、ONNX には x = (q - zero_point) * scale を渡す。
    x を量子化し直すと q に戻ること (往復一致) を確かめてから実行する。
    scale / zero_point は ONNX と stai_network.h の両方から読み、一致を確かめる。

出力:
    10 クラスの float32 (softmax 後)。ONNX Runtime の既定 (QDQ を int8 演算に融合) と
    グラフ最適化なし (float で量子化を模擬) の2通りを表示し、C のヘッダにも入れる。
"""

import argparse
import hashlib
import re
import sys
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
from onnx import numpy_helper

SEED = 2026
SHAPE = (1, 64, 96, 1)  # [バッチ][メル 64][フレーム 96][1]
CLASSES = [  # ST の Projects/Dpu/ai_model_config.h.aed の CTRL_X_CUBE_AI_MODEL_CLASS_LIST の並び
    "chainsaw", "clock_tick", "crackling_fire", "crying_baby", "dog",
    "helicopter", "rain", "rooster", "sea_waves", "sneezing",
]

REPO = Path(__file__).resolve().parent.parent
NPU_DIR = REPO / "mtk3bsp2_stm32n657" / "Appli" / "Application" / "npu"
STAI_H = NPU_DIR / "st" / "model" / "stai_network.h"
OUT_H = NPU_DIR / "aed_test_input.h"


def onnx_input_quant(model: onnx.ModelProto) -> tuple[str, np.float32, int]:
    """入力 → Transpose → QuantizeLinear をたどり、入力名と scale / zero_point を返す。"""
    g = model.graph
    inits = {t.name: numpy_helper.to_array(t) for t in g.initializer}
    consumers: dict[str, list[onnx.NodeProto]] = {}
    for n in g.node:
        for x in n.input:
            consumers.setdefault(x, []).append(n)

    if len(g.input) != 1:
        sys.exit(f"入力が1つでない: {[i.name for i in g.input]}")
    inp = g.input[0]
    tt = inp.type.tensor_type
    dims = tuple(d.dim_value for d in tt.shape.dim)
    if tt.elem_type != onnx.TensorProto.FLOAT or dims != SHAPE:
        sys.exit(f"入力の型が想定と違う: {onnx.helper.printable_type(inp.type)}")

    x = inp.name
    for _ in range(2):  # Transpose の次が QuantizeLinear のはず
        nodes = consumers.get(x, [])
        if len(nodes) != 1:
            break
        n = nodes[0]
        if n.op_type == "QuantizeLinear":
            scale = inits[n.input[1]]
            zp = inits[n.input[2]]
            if scale.size != 1 or zp.dtype != np.int8:
                sys.exit(f"入力の量子化が per-tensor int8 でない: scale={scale}, zp={zp}")
            return inp.name, np.float32(scale.reshape(())), int(zp.reshape(()))
        x = n.output[0]
    sys.exit("入力の直後に QuantizeLinear が見つからない")


def stai_input_quant() -> tuple[np.float32, int]:
    """ボード側 (stai_network.h) の入力の scale / zero_point。"""
    text = STAI_H.read_text(encoding="utf-8")
    s = re.search(r"#define STAI_NETWORK_IN_1_SCALES\s*\\\s*\{\s*\\\s*([-0-9.eE+]+)", text)
    z = re.search(r"#define STAI_NETWORK_IN_1_OFFSETS\s*\\\s*\{\s*\\\s*(-?\d+)", text)
    if not s or not z:
        sys.exit(f"{STAI_H} から入力の scale / zero_point を読めない")
    return np.float32(s.group(1)), int(z.group(1))


def run(path: Path, name: str, x: np.ndarray, optimize: bool) -> np.ndarray:
    so = ort.SessionOptions()
    if not optimize:
        so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    sess = ort.InferenceSession(str(path), so, providers=["CPUExecutionProvider"])
    return sess.run(None, {name: x})[0].reshape(-1).astype(np.float32)


def c_float(v: np.float32) -> str:
    return f"{float(v):.9g}f"


def write_header(q: np.ndarray, y_opt: np.ndarray, y_flt: np.ndarray,
                 scale: np.float32, zp: int, sha256: str) -> None:
    flat = q.reshape(-1)  # C の並び (メルが外側、フレームが内側)
    rows = []
    for i in range(0, flat.size, 16):
        rows.append("\t" + ", ".join(f"{int(v):4d}" for v in flat[i:i + 16]) + ",")
    names = ", ".join(f'"{c}"' for c in CLASSES)
    text = f"""/* 自動生成: scripts/aed_ref.py。手で編集しない (作り直すときはスクリプトを実行する) */
#ifndef NPU_AED_TEST_INPUT_H
#define NPU_AED_TEST_INPUT_H

#include <stdint.h>

/*
 * AED モデルの比較用の固定入力と、ONNX Runtime で求めた期待値 (Phase 1 タスク6 で使う)
 *
 *   モデル: yamnet_1024_64x96_tl_qdq_int8.onnx
 *           sha256 {sha256}
 *   入力:   int8 1x64x96x1。一様乱数 [-128, 127]、numpy default_rng(seed={SEED})
 *           並びは [メル 0..63][フレーム 0..95] (フレームが内側)。NPU の入力バッファ
 *           (npu_rt_input()) にこのままコピーし、D キャッシュを clean+invalidate してから推論する
 *   量子化: scale={float(scale):.9g}, zero_point={zp} (NPU 版と ONNX で一致を確認済み)。
 *           ONNX には (q - zero_point) * scale の float32 を渡した
 *   期待値: ONNX Runtime {ort.__version__} (CPU)。softmax 後の float32
 *
 * static な配列なので、このヘッダを include するのは1つの .c だけにする。
 */

#define AED_TEST_SEED		({SEED})
#define AED_TEST_INPUT_LEN	({flat.size})
#define AED_TEST_CLASSES	({len(CLASSES)})

static const int8_t aed_test_input[AED_TEST_INPUT_LEN] __attribute__((aligned(32))) = {{
{chr(10).join(rows)}
}};

/* ONNX Runtime の既定 (QDQ を int8 演算に融合)。NPU の整数演算に近いのはこちらの見込み */
static const float aed_test_expect_ort[AED_TEST_CLASSES] = {{
	{", ".join(c_float(v) for v in y_opt)}
}};

/* ONNX Runtime のグラフ最適化なし (float で量子化を模擬) */
static const float aed_test_expect_ort_float[AED_TEST_CLASSES] = {{
	{", ".join(c_float(v) for v in y_flt)}
}};

/* クラス名 (出力の並び。ST の ai_model_config.h.aed と同じ) */
static const char *const aed_test_class_names[AED_TEST_CLASSES] = {{
	{names}
}};

#endif	/* NPU_AED_TEST_INPUT_H */
"""
    OUT_H.write_text(text, encoding="utf-8", newline="\n")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("onnx", type=Path, help="yamnet_1024_64x96_tl_qdq_int8.onnx")
    args = ap.parse_args()

    blob = args.onnx.read_bytes()
    sha256 = hashlib.sha256(blob).hexdigest()
    model = onnx.load_from_string(blob)

    name, scale, zp = onnx_input_quant(model)
    b_scale, b_zp = stai_input_quant()
    if scale != b_scale or zp != b_zp:
        sys.exit(f"量子化パラメータが食い違う: ONNX scale={scale!r} zp={zp} / "
                 f"stai_network.h scale={b_scale!r} zp={b_zp}")

    q = np.random.default_rng(SEED).integers(-128, 128, size=SHAPE, dtype=np.int8)
    x = ((q.astype(np.float32) - np.float32(zp)) * scale).astype(np.float32)
    # ONNX の QuantizeLinear と同じ計算 (丸めは偶数丸め) で q に戻るか
    back = np.clip(np.rint(x / scale) + zp, -128, 127).astype(np.int8)
    if not np.array_equal(back, q):
        sys.exit(f"量子化の往復が一致しない ({np.count_nonzero(back != q)} 要素)")

    y_opt = run(args.onnx, name, x, optimize=True)
    y_flt = run(args.onnx, name, x, optimize=False)

    print(f"model : {args.onnx} (sha256 {sha256[:16]}...)")
    print(f"ort   : {ort.__version__}, providers={ort.get_available_providers()}")
    print(f"input : {name} float32 {SHAPE} <- int8 q, seed={SEED}, "
          f"q min/max/mean = {q.min()}/{q.max()}/{q.mean():.2f}")
    print(f"quant : scale={float(scale):.9g} zero_point={zp} "
          "(ONNX == stai_network.h, round-trip OK)")
    print()
    print(f"{'#':>2} {'class':<15} {'ORT default':>12} {'ORT no-opt':>12}")
    for i, c in enumerate(CLASSES):
        print(f"{i:>2} {c:<15} {y_opt[i]:12.6f} {y_flt[i]:12.6f}")
    print(f"   {'sum':<15} {y_opt.sum():12.6f} {y_flt.sum():12.6f}")
    print(f"argmax: {int(y_opt.argmax())} {CLASSES[int(y_opt.argmax())]} (default) / "
          f"{int(y_flt.argmax())} {CLASSES[int(y_flt.argmax())]} (no-opt), "
          f"max |default - no-opt| = {np.abs(y_opt - y_flt).max():.6f}")

    write_header(q, y_opt, y_flt, scale, zp, sha256)
    print(f"\nwrote {OUT_H.relative_to(REPO)}")


if __name__ == "__main__":
    main()
