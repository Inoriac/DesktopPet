"""Validate the distributed embedding model with the project's Windows venv.

This intentionally uses ONNX Runtime only.  ``onnx.checker.check_model`` can
crash inside its native checker on this model with some ONNX/Python builds even
though the runtime loads and executes the graph correctly.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import onnxruntime as ort
from tokenizers import Tokenizer


def embed(session: ort.InferenceSession, tokenizer: Tokenizer, text: str) -> np.ndarray:
    encoded = tokenizer.encode(text)
    # Match the provider's fixed [CLS]/[SEP] and max-sequence behavior.
    ids = [101, *encoded.ids[:62], 102]
    values = np.asarray([ids], dtype=np.int64)
    mask = np.ones_like(values, dtype=np.int64)
    types = np.zeros_like(values, dtype=np.int64)
    hidden = session.run(
        ["last_hidden_state"],
        {"input_ids": values, "token_type_ids": types, "attention_mask": mask},
    )[0][0, : len(ids), :]
    vector = hidden.mean(axis=0)
    vector /= np.linalg.norm(vector)
    return vector


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets", type=Path, default=Path("assets/embeddings"))
    args = parser.parse_args()
    model = args.assets / "model_quantized.onnx"
    tokenizer_path = args.assets / "tokenizer.json"
    if not model.is_file() or not tokenizer_path.is_file():
        raise SystemExit(f"missing model/tokenizer under {args.assets}")

    session = ort.InferenceSession(str(model), providers=["CPUExecutionProvider"])
    tokenizer = Tokenizer.from_file(str(tokenizer_path))
    inputs = {item.name for item in session.get_inputs()}
    expected = {"input_ids", "token_type_ids", "attention_mask"}
    if inputs != expected:
        raise SystemExit(f"unexpected model inputs: {sorted(inputs)}")
    output = next(item for item in session.get_outputs() if item.name == "last_hidden_state")
    if output.shape[-1] != 512:
        raise SystemExit(f"unexpected embedding dimension: {output.shape}")

    anchor = embed(session, tokenizer, "用户喜欢c++编程语言")
    similar = embed(session, tokenizer, "用户偏爱C++程序设计")
    unrelated = embed(session, tokenizer, "今天天气晴朗适合出门散步")
    norm = float(np.linalg.norm(anchor))
    sim = float(anchor @ similar)
    other = float(anchor @ unrelated)
    if not (0.95 < norm < 1.05 and sim > 0.5 and sim > other):
        raise SystemExit(f"embedding assertions failed: norm={norm} similar={sim} unrelated={other}")

    print(f"onnxruntime={ort.__version__}")
    print(f"providers={session.get_providers()}")
    print(f"norm={norm:.6f} similar={sim:.6f} unrelated={other:.6f}")
    print("ONNX validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
