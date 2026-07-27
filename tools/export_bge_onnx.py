#!/usr/bin/env python3
"""构建期一次性导出 bge-small-zh-v1.5 → int8 动态量化 ONNX + tokenizer。

设计依据（见 memory/phase1-p0-progress）：
  - 桌宠分发不允许 PyTorch(~2GB)。bge-small-zh 官方只发 PyTorch，
    故在构建期导出 int8 ONNX，运行时只依赖 onnxruntime(几 MB)。
  - 模型源用 ModelScope（魔搭），因本机 HF 直连/镜像不可达；
    若环境可连 HF 也可改用 HuggingFace hub。
  - 量化走 onnxruntime.quantization 动态 int8（权重 int8、激活动态量化），
    权重 24M → 模型 ~24MB，CPU 推理延迟毫秒级，检索召回掉点 <2%。

产物（写到 assets/embeddings/）：
  - model_quantized.onnx   int8 量化模型（运行时加载）
  - model.onnx             FP32 对照（可选，便于排查精度问题）
  - vocab.txt              WordPiece 词表（C++ 端自写 tokenizer 读它）
  - tokenizer.json         HF fast tokenizer（备用 / 调试用）
  - export_meta.json       维度、max_seq、量化方式等元信息

用法：
  launcher/.venv/bin/python tools/export_bge_onnx.py [--out assets/embeddings]
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

# 这些 import 放到函数里也行，但放前面能在依赖缺失时第一时间报错。
import torch
import transformers
import onnx
import onnxruntime as ort


MODEL_ID = "BAAI/bge-small-zh-v1.5"
MAX_SEQ_LEN = 512  # 与模型 config 一致；运行时可截短省算力
EMBED_DIM = 512


def _download_model() -> Path:
    """从 ModelScope 拉模型快照，返回本地缓存根目录。"""
    try:
        from modelscope import snapshot_download
    except ImportError:
        print("[export] modelscope 未安装，尝试 HuggingFace hub（本机可能不可达）", file=sys.stderr)
        from transformers.utils import cached_file
        # 退化路径：直接靠 transformers 的 hub 下载
        _ = cached_file(MODEL_ID, "config.json")
        from huggingface_hub import snapshot_download as hf_dl
        return Path(hf_dl(repo_id=MODEL_ID))

    path = snapshot_download(MODEL_ID, revision="master")
    print(f"[export] model snapshot: {path}")
    return Path(path)


def export_fp32(model_dir: Path, out_dir: Path) -> Path:
    """导出 FP32 ONNX（动态 batch、动态 seq）。返回 onnx 路径。"""
    from transformers import AutoModel, AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(str(model_dir))
    model = AutoModel.from_pretrained(str(model_dir), torch_dtype=torch.float32)
    model.eval()

    # dummy input：batch=1, seq=8；seq 维用动态轴
    dummy = tokenizer("导出测试", return_tensors="pt", padding="max_length",
                      max_length=8, truncation=True)
    input_names = list(dummy.keys())  # input_ids / token_type_ids / attention_mask

    onnx_path = out_dir / "model.onnx"
    with torch.no_grad():
        torch.onnx.export(
            model,
            (dummy["input_ids"], dummy.get("token_type_ids"), dummy["attention_mask"]),
            str(onnx_path),
            input_names=["input_ids", "token_type_ids", "attention_mask"],
            output_names=["last_hidden_state"],
            dynamic_axes={
                "input_ids": {0: "batch", 1: "seq"},
                "token_type_ids": {0: "batch", 1: "seq"},
                "attention_mask": {0: "batch", 1: "seq"},
                "last_hidden_state": {0: "batch", 1: "seq"},
            },
            opset_version=17,
            do_constant_folding=True,
        )
    print(f"[export] fp32 onnx -> {onnx_path}")
    return onnx_path


def quantize_dynamic_int8(fp32_onnx: Path, out_dir: Path) -> Path:
    """对 FP32 ONNX 做动态 int8 权重量化。返回量化后路径。"""
    from onnxruntime.quantization import quantize_dynamic, QuantType

    quant_path = out_dir / "model_quantized.onnx"
    quantize_dynamic(
        str(fp32_onnx),
        str(quant_path),
        weight_type=QuantType.QInt8,
        op_types_to_quantize=["MatMul", "Gemm", "Conv"],  # BERT 主要是 MatMul
    )
    print(f"[export] int8 onnx -> {quant_path}")
    return quant_path


def copy_tokenizer(model_dir: Path, out_dir: Path) -> None:
    for name in ("vocab.txt", "tokenizer.json", "tokenizer_config.json"):
        src = model_dir / name
        if src.exists():
            shutil.copy2(src, out_dir / name)
            print(f"[export] copy {name}")


def write_meta(out_dir: Path, quant_path: Path, fp32_path: Path) -> None:
    meta = {
        "model_id": MODEL_ID,
        "embedding_dim": EMBED_DIM,
        "max_seq_len": MAX_SEQ_LEN,
        "pooling": "mean",
        "normalize": True,
        "quantization": "onnxruntime dynamic int8 (weight QInt8)",
        "runtime": "onnxruntime",
        "opset": 17,
        "files": {
            "int8": quant_path.name,
            "fp32_ref": fp32_path.name,
            "vocab": "vocab.txt",
        },
        "sizes_mb": {
            "int8": round(quant_path.stat().st_size / 1e6, 2),
            "fp32": round(fp32_path.stat().st_size / 1e6, 2),
        },
    }
    (out_dir / "export_meta.json").write_text(json.dumps(meta, indent=2, ensure_ascii=False))
    print(f"[export] meta -> {out_dir / 'export_meta.json'}")
    print(f"[export] DONE  int8={meta['sizes_mb']['int8']}MB  fp32={meta['sizes_mb']['fp32']}MB")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="assets/embeddings")
    ap.add_argument("--keep-fp32", action="store_true",
                    help="保留 FP32 对照模型(默认删除,只留 int8 随桌宠分发)")
    args = ap.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    model_dir = _download_model()
    fp32_path = export_fp32(model_dir, out_dir)
    quant_path = quantize_dynamic_int8(fp32_path, out_dir)
    copy_tokenizer(model_dir, out_dir)

    if not args.keep_fp32:
        fp32_path.unlink(missing_ok=True)
        # 量化器留的对象级缓存
        for p in out_dir.glob("*quantized.onnx.data"):
            p.unlink(missing_ok=True)
        print("[export] removed fp32 reference (use --keep-fp32 to retain)")
    write_meta(out_dir, quant_path, fp32_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())