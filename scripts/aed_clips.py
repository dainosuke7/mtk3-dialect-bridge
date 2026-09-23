#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["onnxruntime==1.30.0", "onnx==1.23.0", "numpy==2.5.3", "scipy==1.18.1"]
# ///
"""ESC-10 の実録音を ST と同じ前処理で int8 入力にし、ONNX Runtime の1位と一緒に C ヘッダへ出す。

Phase 1 タスク6: NPU の推論が正しいかを、実録音 30 本の1位クラスを PC と比べて判定する。
ここで作る log-mel の Python 実装は、タスク8 でボード側に前処理を移植するときの参照にもなる。

使い方 (uv が依存パッケージを用意する):
    uv run scripts/aed_clips.py <STM32N6-GettingStarted-Audio> <ESC-50>

    <STM32N6-GettingStarted-Audio>: v2.3.0 のリポジトリ (モデルと前処理の設定・表を読む)
    <ESC-50>: github.com/karolpiczak/ESC-50 のリポジトリ (meta/esc50.csv と下の 30 本の wav)

出力: mtk3bsp2_stm32n657/Appli/Application/npu/aed_test_clips.h
      30 本の int8 入力と PC の1位 (NPU の推論の判定用。npu_selftest.c の clips_check)。
      ROM が足りないときは npu_selftest.c の AED_USE_TEST_CLIPS を 0 にしてビルドから外す
    mtk3bsp2_stm32n657/Appli/Application/aed/aed_ref_clips.h
      上のうち2本ぶんの生 PCM も入れたもの (ボードの前処理を突き合わせる用。タスク8。
      infer_task.c の preproc_test)
    どちらも ESC-50 由来のデータを含むのでリポジトリには入れない (.gitignore 済み)。
    無ければボード側はその確認を飛ばす (__has_include)。

前処理 (ST の GettingStarted-Audio と同じ。確認箇所は各関数のコメント):
    int16 16kHz の先頭 15600 サンプル → 96 列 (ホップ 160、窓 400)
    列ごとに: /32768 → 周期ハン窓 400 → 左右 56 ずつゼロ詰めして 512 点 rfft → 振幅 |X|
            → メルフィルタ 64 本 (HTK、125〜7500Hz、正規化なし) → 0 以下は FLT_MIN → 自然対数
            → int8 = SSAT(roundf(logmel / scale + zero_point))   scale / zero_point はモデルの入力
    並び: [メル 0..63][列 0..95] (列が内側)
    ST は FP16 で計算するが、ここは float32 (学習時の Python 前処理に近い側)。差は int8 で 1 LSB 程度
"""

import argparse
import csv
import hashlib
import re
import sys
import wave
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
from onnx import numpy_helper
from scipy.signal import resample_poly

# ---------------------------------------------------------------------------
# 前処理のパラメータ。ST の Projects/Dpu/ai_model_config.h.aed と一致するかを起動時に確かめる
# (GenHeader/user_config_aed.yaml から生成されたもの)
SR = 16000
N_MELS = 64
N_COLS = 96
HOP = 160
WIN = 400
N_FFT = 512
FMIN = 125
FMAX = 7500
N_SAMPLES = HOP * (N_COLS - 1) + WIN          # 15600
PAD_L = (N_FFT - WIN) // 2                     # 56 (preproc_dpu.c の pad_left)
PAD_R = (N_FFT - WIN) // 2 + (N_FFT - WIN) % 2  # 56 (pad_right)

# 出力の並び。ST の gen_h_file.py がクラス名をソートして CLASS_LIST を作る
CLASSES = ["chainsaw", "clock_tick", "crackling_fire", "crying_baby", "dog",
           "helicopter", "rain", "rooster", "sea_waves", "sneezing"]

# ---------------------------------------------------------------------------
# 使うクリップ (ESC-50 の ESC-10 サブセット、fold 5)。
# 選び方: クラスごとにファイル名順で、先頭 15600 サンプル (16kHz) の RMS がクリップ全体の
# RMS から 6dB 以内 (= 最初の約1秒に音がある) のものを、元の録音 (src_file) が違うものを
# 優先して 3 本。fold 5 に元の録音が2つしかない crying_baby だけ同じ録音から2本。
# 条件はこのスクリプトでも毎回確かめ、外れたら警告する
CLIPS = {
    "chainsaw":       ["5-170338-A-41.wav", "5-171653-A-41.wav", "5-185579-A-41.wav"],
    "clock_tick":     ["5-201194-A-38.wav", "5-208624-A-38.wav", "5-209698-A-38.wav"],
    "crackling_fire": ["5-186924-A-12.wav", "5-189212-A-12.wav", "5-189237-A-12.wav"],
    "crying_baby":    ["5-151085-A-20.wav", "5-198411-B-20.wav", "5-198411-C-20.wav"],
    "dog":            ["5-203128-A-0.wav", "5-208030-A-0.wav", "5-212454-A-0.wav"],
    "helicopter":     ["5-177957-A-40.wav", "5-191131-A-40.wav", "5-205898-A-40.wav"],
    "rain":           ["5-181766-A-10.wav", "5-188655-A-10.wav", "5-193339-A-10.wav"],
    "rooster":        ["5-194930-A-1.wav", "5-200334-A-1.wav", "5-233160-A-1.wav"],
    "sea_waves":      ["5-200461-A-11.wav", "5-208810-B-11.wav", "5-213077-A-11.wav"],
    "sneezing":       ["5-187979-A-21.wav", "5-194533-A-21.wav", "5-202220-A-21.wav"],
}
LEVEL_MARGIN_DB = 6.0

# ---------------------------------------------------------------------------
# ボードの前処理を突き合わせる2本 (タスク8)。生 PCM (15600 x int16 = 30.5KB) も
# ヘッダに入れるので本数を絞る。
# 選び方: 上の CLIPS のうち PC の1位が正解と一致して最適化あり・なしで同じ、かつ確率が
# REF_MIN_PROB 以上のものから、音量と時間構造が対照的な2本
#   5-203128-A-0.wav  (dog,        -16dBFS) 大きい音・短い立ち上がりが並ぶ
#   5-201194-A-38.wav (clock_tick, -38dBFS) 小さい音・ほぼ無音の中に点在する (量子化の下限側を通る)
# 条件はこのスクリプトでも毎回確かめ、外れたら警告する
REF_CLIPS = ["5-203128-A-0.wav", "5-201194-A-38.wav"]
REF_MIN_PROB = 0.999

REPO = Path(__file__).resolve().parent.parent
OUT_H = REPO / "mtk3bsp2_stm32n657" / "Appli" / "Application" / "npu" / "aed_test_clips.h"
SELFTEST_C = OUT_H.parent / "npu_selftest.c"
INFER_TASK_C = OUT_H.parent / "infer_task.c"
REF_OUT_H = OUT_H.parent.parent / "aed" / "aed_ref_clips.h"
MODEL_REL = Path("Projects/X-CUBE-AI/models/yamnet_1024_64x96_tl_qdq_int8.onnx")
CONFIG_REL = Path("Projects/Dpu/ai_model_config.h.aed")
TABLES_REL = Path("Projects/Dpu/user_mel_tables.c.aed")


# ---------------------------------------------------------------------------
# ST の設定・表との照合

def check_st_config(path: Path) -> None:
    """ai_model_config.h.aed の値がこのスクリプトの定数と同じか。"""
    text = path.read_text(encoding="utf-8", errors="replace")

    def define(name: str) -> str:
        m = re.search(rf"#define\s+{name}\s+(.+?)\s*(?:/\*|$)", text, re.M)
        if not m:
            sys.exit(f"{path}: {name} が無い")
        return m.group(1).strip()

    expect = {
        "CTRL_X_CUBE_AI_SENSOR_ODR": "(16000.0F)",
        "CTRL_X_CUBE_AI_PREPROC": "(CTRL_AI_SPECTROGRAM_LOG_MEL)",
        "CTRL_X_CUBE_AI_SPECTROGRAM_NMEL": f"({N_MELS}U)",
        "CTRL_X_CUBE_AI_SPECTROGRAM_COL": f"({N_COLS}U)",
        "CTRL_X_CUBE_AI_SPECTROGRAM_HOP_LENGTH": f"({HOP}U)",
        "CTRL_X_CUBE_AI_SPECTROGRAM_NFFT": f"({N_FFT}U)",
        "CTRL_X_CUBE_AI_SPECTROGRAM_WINDOW_LENGTH": f"({WIN}U)",
        "CTRL_X_CUBE_AI_SPECTROGRAM_NORMALIZE": "(0U)",
        "CTRL_X_CUBE_AI_SPECTROGRAM_FORMULA": "(MEL_HTK)",
        "CTRL_X_CUBE_AI_SPECTROGRAM_FMIN": f"({FMIN}U)",
        "CTRL_X_CUBE_AI_SPECTROGRAM_FMAX": f"({FMAX}U)",
        "CTRL_X_CUBE_AI_SPECTROGRAM_TYPE": "(SPECTRUM_TYPE_MAGNITUDE)",
        "CTRL_X_CUBE_AI_SPECTROGRAM_LOG_FORMULA": "(LOGMELSPECTROGRAM_SCALE_LOG)",
    }
    for name, value in expect.items():
        got = define(name)
        if got != value:
            sys.exit(f"{path}: {name} = {got} (このスクリプトは {value} を前提にしている)")
    m = re.search(r"CLASS_LIST\s+\{(.+?)\}", text, re.S)
    names = re.findall(r'"([^"]+)"', m.group(1)) if m else []
    if names != CLASSES:
        sys.exit(f"{path}: クラスの並びが違う: {names}")


def hann_periodic(n: int) -> np.ndarray:
    """librosa.filters.get_window('hann', n) (fftbins=True なので周期ハン窓)。
    ST の lookup_tables_generator.py:generate_hann_window_LUT と同じ"""
    k = np.arange(n, dtype=np.float64)
    return (0.5 - 0.5 * np.cos(2.0 * np.pi * k / n)).astype(np.float32)


def mel_filterbank() -> np.ndarray:
    """librosa.filters.mel(sr, n_fft, n_mels, fmin, fmax, htk=True, norm=None) と同じ計算。
    ST の lookup_tables_generator.py:generate_mel_LUTs が使う (戻り値は float32、64 x 257)"""
    hz_to_mel = lambda f: 2595.0 * np.log10(1.0 + f / 700.0)
    mel_to_hz = lambda m: 700.0 * (10.0 ** (m / 2595.0) - 1.0)
    fftfreqs = np.linspace(0.0, SR / 2.0, 1 + N_FFT // 2)
    mel_f = mel_to_hz(np.linspace(hz_to_mel(FMIN), hz_to_mel(FMAX), N_MELS + 2))
    fdiff = np.diff(mel_f)
    ramps = np.subtract.outer(mel_f, fftfreqs)
    weights = np.zeros((N_MELS, 1 + N_FFT // 2), dtype=np.float32)
    for i in range(N_MELS):
        lower = -ramps[i] / fdiff[i]
        upper = ramps[i + 2] / fdiff[i + 1]
        weights[i] = np.maximum(0.0, np.minimum(lower, upper))
    return weights


def check_st_tables(path: Path, win: np.ndarray, fb: np.ndarray) -> str:
    """user_mel_tables.c.aed (ST が生成した表) とここで作った表を比べる。"""
    text = path.read_text(encoding="utf-8", errors="replace")

    def array(name: str) -> np.ndarray:
        m = re.search(rf"{name}\[\d+\]\s*=\s*\{{(.*?)\}};", text, re.S)
        if not m:
            sys.exit(f"{path}: {name} が無い")
        return np.array([float(v.rstrip("Ff")) for v in re.findall(r"[-0-9.eE+]+[Ff]?", m.group(1))])

    st_win = array("user_win")
    st_lut = array("user_melFiltersLut")
    st_start = array("user_melFiltersStartIndices").astype(int)
    st_stop = array("user_melFiltersStopIndices").astype(int)

    start = np.array([np.nonzero(r)[0][0] for r in fb])
    stop = np.array([np.nonzero(r)[0][-1] for r in fb])
    lut = fb[np.nonzero(fb)]
    if not (np.array_equal(start, st_start) and np.array_equal(stop, st_stop) and lut.size == st_lut.size):
        sys.exit("メルフィルタの非ゼロ範囲が ST の表と違う")
    d_win = float(np.abs(win - st_win).max())
    d_lut = float(np.abs(lut - st_lut).max())
    if d_win > 1e-7 or d_lut > 1e-7:
        sys.exit(f"ST の表との差が大きい: window {d_win:.3g}, mel {d_lut:.3g}")
    return (f"window 400 max diff {d_win:.2g}, mel LUT {lut.size} coefs max diff {d_lut:.2g}, "
            f"start/stop indices identical")


# ---------------------------------------------------------------------------
# 前処理 (ST の C 実装の float32 版)

def roundf(v: np.ndarray) -> np.ndarray:
    """C の roundf (0.5 は 0 から遠い側へ)。numpy の rint (偶数丸め) とは違う"""
    return np.sign(v) * np.floor(np.abs(v) + 0.5)


def logmel_q8(x: np.ndarray, win: np.ndarray, fb: np.ndarray,
              inv_scale: np.float32, zp: int) -> np.ndarray:
    """int16 x[15600] → int8 [64][96]。
    ST の対応: preproc_dpu.c:135-143 (列のループと転置)、preproc_dpu.c:52-53,68-70 (ゼロ詰め・Ref・TopdB)、
    feature_extraction_f16.c:264-347 LogMelSpectrogramColumn_q15_f16_Q8 (量子化は :334-337)、
    audio_din_f16.c:30-41 audio_is16of16_pad (arm_q15_to_f16 = /32768)、
    feature_extraction_f16.c:73 SpectrogramColumn_f16 (MAGNITUDE)、mel_filterbank_f16.c:214 MelFilterbank_f16"""
    out = np.empty((N_MELS, N_COLS), dtype=np.int8)
    for i in range(N_COLS):
        frame = x[HOP * i: HOP * i + WIN].astype(np.float32) / np.float32(32768.0)
        buf = np.concatenate([np.zeros(PAD_L, np.float32), frame * win, np.zeros(PAD_R, np.float32)])
        mag = np.abs(np.fft.rfft(buf)).astype(np.float32)          # 257 本
        mel = (fb @ mag).astype(np.float32)                         # /ref (1.0) は省略
        mel = np.where(mel <= 0.0, np.float32(np.finfo(np.float32).tiny), mel)
        logmel = np.log(mel).astype(np.float32)
        q = roundf(logmel * inv_scale + np.float32(zp))
        out[:, i] = np.clip(q, -128, 127).astype(np.int8)          # __SSAT(.., 8)
    return out


def read_wav_16k(path: Path) -> tuple[np.ndarray, float, float]:
    """ESC-50 の wav (44.1kHz mono int16) を 16kHz int16 にし、先頭 15600 サンプルと、
    先頭部分・全体の RMS (dBFS) を返す"""
    with wave.open(str(path)) as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (44100, 1, 2):
            sys.exit(f"{path}: 44.1kHz mono 16bit でない")
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    y = resample_poly(x, 160, 441)
    y16 = np.clip(np.rint(y), -32768, 32767).astype(np.int16)
    db = lambda v: 20.0 * np.log10(max(float(np.sqrt(np.mean((v / 32768.0) ** 2))), 1e-12))
    return y16[:N_SAMPLES], db(y16[:N_SAMPLES].astype(np.float64)), db(y16.astype(np.float64))


# ---------------------------------------------------------------------------
# ONNX

def onnx_input(model: onnx.ModelProto) -> tuple[str, np.float32, int]:
    """入力名と、入力直後 (Transpose の次) の QuantizeLinear の scale / zero_point。"""
    g = model.graph
    inits = {t.name: numpy_helper.to_array(t) for t in g.initializer}
    consumers: dict[str, list[onnx.NodeProto]] = {}
    for n in g.node:
        for x in n.input:
            consumers.setdefault(x, []).append(n)
    x = g.input[0].name
    for _ in range(2):
        n = consumers[x][0]
        if n.op_type == "QuantizeLinear":
            return g.input[0].name, np.float32(inits[n.input[1]].reshape(())), int(inits[n.input[2]].reshape(()))
        x = n.output[0]
    sys.exit("入力の直後に QuantizeLinear が無い")


def sessions(model_bytes: bytes) -> dict[str, ort.InferenceSession]:
    out = {}
    for mode, level in (("ort", ort.GraphOptimizationLevel.ORT_ENABLE_ALL),
                        ("noopt", ort.GraphOptimizationLevel.ORT_DISABLE_ALL)):
        so = ort.SessionOptions()
        so.graph_optimization_level = level
        out[mode] = ort.InferenceSession(model_bytes, so, providers=["CPUExecutionProvider"])
    return out


# ---------------------------------------------------------------------------

def c_rows(values, per_line: int, indent: str = "\t\t") -> str:
    """整数の配列を1行 per_line 個で並べる (末尾のカンマは C では問題ない)"""
    return "\n".join(indent + ",".join(str(int(v)) for v in values[i:i + per_line]) + ","
                     for i in range(0, len(values), per_line))


def write_ref_header(clips: list[dict], sha256: str, scale: np.float32, zp: int, tables_note: str) -> list[dict]:
    """前処理をボードで突き合わせる2本 (生 PCM + PC の int8 テンソル + PC の1位) を出す。"""
    by_file = {c["file"]: c for c in clips}
    ref = []
    for fn in REF_CLIPS:
        c = by_file.get(fn)
        if c is None:
            sys.exit(f"REF_CLIPS の {fn} が CLIPS に無い")
        if c["top_ort"] != c["truth"] or c["top_noopt"] != c["truth"]:
            print(f"(!) {fn}: PC の1位が正解と違う (前処理の突き合わせには向かない)")
        elif min(c["p_ort"], c["p_noopt"]) < REF_MIN_PROB:
            print(f"(!) {fn}: PC の確率が {min(c['p_ort'], c['p_noopt']):.3f} < {REF_MIN_PROB}"
                  f" (1位が入れ替わりやすい)")
        ref.append(c)

    arr = lambda key, fmt: ", ".join(fmt(c[key]) for c in ref)
    pcm = "\n".join(f"\t{{ /* {c['file']} ({c['label']}) */\n" + c_rows(c["pcm"], 16) + "\n\t}," for c in ref)
    ten = "\n".join(f"\t{{ /* {c['file']} ({c['label']}) */\n" + c_rows(c["q"].reshape(-1), 32) + "\n\t}," for c in ref)
    text = f"""/* 自動生成: scripts/aed_clips.py。手で編集しない。ESC-50 由来のデータを含むのでコミットしない */
#ifndef AED_REF_CLIPS_H
#define AED_REF_CLIPS_H

#include <stdint.h>

/*
 * ボードの前処理 (Application/aed/preproc.c) を PC と突き合わせるための実録音 {len(ref)} 本
 * (Phase 1 タスク8。使うのは infer_task.c の preproc_test)
 *   音声:   ESC-50 (K. J. Piczak, github.com/karolpiczak/ESC-50) の ESC-10 サブセット、fold 5。
 *           ESC-10 は CC BY 3.0、ESC-50 全体は CC BY-NC 3.0 (各クリップの出典は ESC-50 の LICENSE)
 *   pcm:    16kHz int16 に直した先頭 {N_SAMPLES} サンプル (ボードの前処理に入れる生の音)
 *   tensor: その pcm を PC が log-mel にした int8 (scripts/aed_clips.py の logmel_q8)。
 *           ボードの前処理の結果と 1 バイトずつ比べる。並びは [メル 0..63][列 0..95]
 *           {tables_note}
 *   量子化: scale={float(scale):.9g}, zero_point={zp} (Application/aed/preproc.h の値と一致すること)
 *   1位:    上の tensor を入れたときの ONNX Runtime {ort.__version__} (CPU) の1位と確率。
 *           ort = 既定の最適化、noopt = 最適化なし
 *   モデル: yamnet_1024_64x96_tl_qdq_int8.onnx sha256 {sha256}
 * static な配列なので、include するのは1つの .c だけにする。
 */

#define AED_REF_CLIP_COUNT	({len(ref)})
#define AED_REF_SAMPLES		({N_SAMPLES})
#define AED_REF_TENSOR_LEN	({N_MELS * N_COLS})
#define AED_REF_CLASSES		({len(CLASSES)})
#define AED_REF_SCALE		({float(scale):.9g}f)
#define AED_REF_ZP		({zp})

/* 前処理に入れる生の音 (16kHz int16) */
static const int16_t aed_ref_pcm[AED_REF_CLIP_COUNT][AED_REF_SAMPLES] = {{
{pcm}
}};

/* PC が同じ pcm から作った int8 テンソル (ボードの前処理の答え合わせ) */
static const int8_t aed_ref_tensor[AED_REF_CLIP_COUNT][AED_REF_TENSOR_LEN] __attribute__((aligned(32))) = {{
{ten}
}};

static const char *const aed_ref_file[AED_REF_CLIP_COUNT] = {{
	{arr("file", lambda v: f'"{v}"')}
}};

/* 正解のクラス番号 (ESC-50 のラベル) */
static const uint8_t aed_ref_truth[AED_REF_CLIP_COUNT] = {{
	{arr("truth", str)}
}};

/* 上の tensor での PC の1位のクラス番号と、その確率 */
static const uint8_t aed_ref_top_ort[AED_REF_CLIP_COUNT] = {{
	{arr("top_ort", str)}
}};
static const uint8_t aed_ref_top_noopt[AED_REF_CLIP_COUNT] = {{
	{arr("top_noopt", str)}
}};
static const float aed_ref_prob_ort[AED_REF_CLIP_COUNT] = {{
	{arr("p_ort", lambda v: f"{v:.6f}f")}
}};
static const float aed_ref_prob_noopt[AED_REF_CLIP_COUNT] = {{
	{arr("p_noopt", lambda v: f"{v:.6f}f")}
}};

/* クラス名 (並びは aed_test_input.h の aed_test_class_names と同じ) */
static const char *const aed_ref_class_names[AED_REF_CLASSES] = {{
	{", ".join(f'"{n}"' for n in CLASSES)}
}};

#endif	/* AED_REF_CLIPS_H */
"""
    REF_OUT_H.parent.mkdir(parents=True, exist_ok=True)
    REF_OUT_H.write_text(text, encoding="utf-8", newline="\n")
    return ref


def write_header(clips: list[dict], sha256: str, scale: np.float32, zp: int, tables_note: str) -> None:
    lines = [f"\t{{ /* {c['file']} ({c['label']}) */\n" + c_rows(c["q"].reshape(-1), 32) + "\n\t},"
             for c in clips]
    arr = lambda key, fmt: ", ".join(fmt(c[key]) for c in clips)
    text = f"""/* 自動生成: scripts/aed_clips.py。手で編集しない。ESC-50 由来のデータを含むのでコミットしない */
#ifndef NPU_AED_TEST_CLIPS_H
#define NPU_AED_TEST_CLIPS_H

#include <stdint.h>

/*
 * ESC-10 の実録音 {len(clips)} 本を ST と同じ前処理で int8 にした NPU の入力と、ONNX Runtime の1位
 *   音声:   ESC-50 (K. J. Piczak, github.com/karolpiczak/ESC-50) の ESC-10 サブセット、fold 5。
 *           ESC-10 は CC BY 3.0、ESC-50 全体は CC BY-NC 3.0 (各クリップの出典は ESC-50 の LICENSE)
 *   前処理: 16kHz 先頭 15600 サンプル、log-mel 64 x 96 (scripts/aed_clips.py の logmel_q8)
 *           {tables_note}
 *   量子化: scale={float(scale):.9g}, zero_point={zp}。並びは [メル 0..63][列 0..95]
 *   モデル: yamnet_1024_64x96_tl_qdq_int8.onnx sha256 {sha256}
 *   PC:     ONNX Runtime {ort.__version__} (CPU)。ort = 既定の最適化、noopt = 最適化なし
 * クラス番号の並びは aed_test_input.h の aed_test_class_names と同じ。
 * static な配列なので、include するのは1つの .c だけにする。
 */

#define AED_CLIP_COUNT	({len(clips)})

static const int8_t aed_clip_input[AED_CLIP_COUNT][{N_MELS * N_COLS}] __attribute__((aligned(32))) = {{
{chr(10).join(lines)}
}};

static const char *const aed_clip_file[AED_CLIP_COUNT] = {{
	{arr("file", lambda v: f'"{v}"')}
}};

/* 正解のクラス番号 (ESC-50 のラベル) */
static const uint8_t aed_clip_truth[AED_CLIP_COUNT] = {{
	{arr("truth", str)}
}};

/* PC の1位のクラス番号と、その確率 */
static const uint8_t aed_clip_top_ort[AED_CLIP_COUNT] = {{
	{arr("top_ort", str)}
}};
static const uint8_t aed_clip_top_noopt[AED_CLIP_COUNT] = {{
	{arr("top_noopt", str)}
}};
static const float aed_clip_prob_ort[AED_CLIP_COUNT] = {{
	{arr("p_ort", lambda v: f"{v:.6f}f")}
}};
static const float aed_clip_prob_noopt[AED_CLIP_COUNT] = {{
	{arr("p_noopt", lambda v: f"{v:.6f}f")}
}};

#endif	/* NPU_AED_TEST_CLIPS_H */
"""
    OUT_H.write_text(text, encoding="utf-8", newline="\n")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("gs_audio", type=Path, help="STM32N6-GettingStarted-Audio (v2.3.0) のディレクトリ")
    ap.add_argument("esc50", type=Path, help="ESC-50 のディレクトリ")
    args = ap.parse_args()

    check_st_config(args.gs_audio / CONFIG_REL)
    win = hann_periodic(WIN)
    fb = mel_filterbank()
    tables_note = check_st_tables(args.gs_audio / TABLES_REL, win, fb)

    blob = (args.gs_audio / MODEL_REL).read_bytes()
    sha256 = hashlib.sha256(blob).hexdigest()
    name, scale, zp = onnx_input(onnx.load_from_string(blob))
    inv_scale = np.float32(1.0) / scale        # ai_dpu.c:170 の 1 / scale (ST は FP16 に丸める)
    sess = sessions(blob)

    meta = {r["filename"]: r for r in csv.DictReader(open(args.esc50 / "meta" / "esc50.csv", encoding="utf-8"))}

    print(f"ST config  : {CONFIG_REL} matches (16kHz, {N_MELS} mel x {N_COLS} col, hop {HOP}, "
          f"win {WIN}, nfft {N_FFT}, HTK {FMIN}-{FMAX}Hz, magnitude, log)")
    print(f"ST tables  : {tables_note}")
    print(f"model      : sha256 {sha256[:16]}..., input {name} scale={float(scale):.9g} zp={zp}")
    print()
    print(f"{'#':>2} {'file':<20} {'truth':<15} {'lvl/all dB':>10} {'PC ort':<15} {'p':>6} {'PC noopt':<15} {'p':>6}")

    clips = []
    for label in CLASSES:
        for fn in CLIPS[label]:
            r = meta.get(fn)
            if r is None or r["category"] != label or r["esc10"] != "True":
                sys.exit(f"{fn}: ESC-50 のメタデータと合わない")
            path = args.esc50 / "audio" / fn
            if not path.exists():
                sys.exit(f"{path} が無い (ESC-50 の audio/ を取得すること)")
            x, lvl, lvl_all = read_wav_16k(path)
            warn = " (!) quiet start" if lvl < lvl_all - LEVEL_MARGIN_DB else ""
            q = logmel_q8(x, win, fb, inv_scale, zp)
            xin = ((q.astype(np.float32) - np.float32(zp)) * scale).reshape(1, N_MELS, N_COLS, 1)
            c = {"file": fn, "label": label, "truth": CLASSES.index(label), "q": q, "pcm": x}
            for mode, s in sess.items():
                y = s.run(None, {name: xin})[0].reshape(-1)
                c[f"top_{mode}"] = int(y.argmax())
                c[f"p_{mode}"] = float(y.max())
            clips.append(c)
            print(f"{len(clips) - 1:>2} {fn:<20} {label:<15} {lvl:4.0f}/{lvl_all:<4.0f}  "
                  f"{CLASSES[c['top_ort']]:<15} {c['p_ort']:6.3f} {CLASSES[c['top_noopt']]:<15} {c['p_noopt']:6.3f}{warn}")

    n = len(clips)
    agree = sum(c["top_ort"] == c["top_noopt"] for c in clips)
    acc_ort = sum(c["top_ort"] == c["truth"] for c in clips)
    acc_noopt = sum(c["top_noopt"] == c["truth"] for c in clips)
    print()
    print(f"PC top-1 == truth: ort {acc_ort}/{n}, noopt {acc_noopt}/{n}; ort == noopt: {agree}/{n}")

    write_header(clips, sha256, scale, zp, tables_note)
    ref = write_ref_header(clips, sha256, scale, zp, tables_note)
    # ボード側はヘッダが無いと __has_include で読まない。ヘッダ無しでビルドした後だと
    # 依存関係 (.d) にヘッダが載っておらず make が作り直さないので、.c の更新時刻を進める
    SELFTEST_C.touch()
    INFER_TASK_C.touch()
    print(f"wrote {OUT_H.relative_to(REPO)} ({n} x {N_MELS * N_COLS} B), touched {SELFTEST_C.name}")
    print(f"wrote {REF_OUT_H.relative_to(REPO)} ({len(ref)} x ({N_SAMPLES} x int16 + {N_MELS * N_COLS} B): "
          f"{', '.join(c['file'] for c in ref)}), touched {INFER_TASK_C.name}")


if __name__ == "__main__":
    main()
