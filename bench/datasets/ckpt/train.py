#!/usr/bin/env python3
"""Fine-tunes distilgpt2 and saves checkpoints for E8 (BENCHMARKS.md, D97).

    train.py --method full|partial|lora --seed N --model DIR --corpus FILE --out DIR
             [--device cpu|cuda] [--steps 300] [--save-every 50]

Uses only torch, transformers, and safetensors, as installed in Explorer's anaconda3/2024.06
module, so nothing has to be installed. The loop is AdamW with linear warm-up and decay. Each
checkpoint-<step>/ directory has the files Hugging Face's Trainer writes, so the dedup study sees a
realistic layout: model.safetensors (or adapter_model.safetensors), optimizer.pt, scheduler.pt,
rng_state.pth, trainer_state.json, config.json, generation_config.json, and the tokenizer files.

full: every parameter trains. partial: only transformer blocks 4 and 5 of 0-5 train (the token
embeddings, tied to the output layer, stay frozen). lora: rank-8 adapters on each block's
attention projection (c_attn); the base model is frozen and not saved, as with LoRA tooling.
"""
import argparse
import json
import os
import random

import torch
from safetensors.torch import save_file
from transformers import AutoModelForCausalLM, AutoTokenizer


class LoRA(torch.nn.Module):
    """x -> base(x) + (x @ A @ B) * alpha / r, around GPT-2's Conv1D (weight of shape in x out)."""

    def __init__(self, base, r=8, alpha=16):
        super().__init__()
        self.base = base
        n_in, n_out = base.weight.shape
        self.lora_A = torch.nn.Parameter(torch.randn(n_in, r) / r)
        self.lora_B = torch.nn.Parameter(torch.zeros(r, n_out))  # starts as the identity change
        self.scale = alpha / r

    def forward(self, x):
        return self.base(x) + (x @ self.lora_A @ self.lora_B) * self.scale


def blocks(tokenizer, path, size):
    with open(path, encoding="utf-8", errors="replace") as f:
        ids = tokenizer(f.read(), return_attention_mask=False)["input_ids"]
    return [ids[i:i + size] for i in range(0, len(ids) - size, size)]


def save(out, step, method, model, tokenizer, optimizer, scheduler, losses):
    d = os.path.join(out, "checkpoint-%d" % step)
    os.makedirs(d, exist_ok=True)
    if method == "lora":
        adapters = {n: p.detach().cpu().contiguous() for n, p in model.named_parameters()
                    if "lora_" in n}
        save_file(adapters, os.path.join(d, "adapter_model.safetensors"))
        with open(os.path.join(d, "adapter_config.json"), "w") as f:
            json.dump({"r": 8, "lora_alpha": 16, "target_modules": ["c_attn"],
                       "base_model_name_or_path": "distilbert/distilgpt2"}, f, indent=2)
    else:
        model.save_pretrained(d, safe_serialization=True)
    tokenizer.save_pretrained(d)
    torch.save(optimizer.state_dict(), os.path.join(d, "optimizer.pt"))
    torch.save(scheduler.state_dict(), os.path.join(d, "scheduler.pt"))
    rng = {"python": random.getstate(), "torch": torch.get_rng_state()}
    if torch.cuda.is_available():
        rng["cuda"] = torch.cuda.get_rng_state_all()
    torch.save(rng, os.path.join(d, "rng_state.pth"))
    with open(os.path.join(d, "trainer_state.json"), "w") as f:
        json.dump({"global_step": step, "log_history": losses}, f, indent=2)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--method", choices=["full", "partial", "lora"], required=True)
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--model", required=True)
    p.add_argument("--corpus", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--device", choices=["cpu", "cuda"], default="cpu")
    p.add_argument("--steps", type=int, default=300)
    p.add_argument("--save-every", type=int, default=50)
    p.add_argument("--batch", type=int, default=8)
    p.add_argument("--block", type=int, default=256)
    a = p.parse_args()
    if a.device == "cuda" and not torch.cuda.is_available():
        # Fail rather than fall back: CPU training on a shared node took over 8 hours (job 10930678).
        raise SystemExit("--device cuda, but torch %s sees no usable GPU" % torch.__version__)
    random.seed(a.seed)
    torch.manual_seed(a.seed)
    torch.set_num_threads(int(os.environ.get("OMP_NUM_THREADS", "8")))
    tokenizer = AutoTokenizer.from_pretrained(a.model)
    model = AutoModelForCausalLM.from_pretrained(a.model)
    if a.method == "partial":
        for name, param in model.named_parameters():
            param.requires_grad = name.startswith(("transformer.h.4.", "transformer.h.5."))
    elif a.method == "lora":
        for param in model.parameters():
            param.requires_grad = False
        for block in model.transformer.h:
            block.attn.c_attn = LoRA(block.attn.c_attn)
    model.to(a.device)
    trainable = [x for x in model.parameters() if x.requires_grad]
    print("method %s seed %d: %d of %d parameters train" %
          (a.method, a.seed, sum(x.numel() for x in trainable), sum(x.numel() for x in model.parameters())),
          flush=True)
    data = blocks(tokenizer, a.corpus, a.block)
    order = torch.randperm(len(data), generator=torch.Generator().manual_seed(a.seed)).tolist()
    lr = 3e-4 if a.method == "lora" else 5e-5
    optimizer = torch.optim.AdamW(trainable, lr=lr)
    warmup = max(1, min(20, a.steps // 10))
    decay = max(1, a.steps - warmup)
    scheduler = torch.optim.lr_scheduler.LambdaLR(
        optimizer, lambda s: min((s + 1) / warmup, max(0.0, (a.steps - s) / decay)))
    model.train()
    losses = []
    for step in range(1, a.steps + 1):
        start = ((step - 1) * a.batch) % len(order)
        ids = torch.tensor([data[order[(start + i) % len(order)]] for i in range(a.batch)],
                           device=a.device)
        loss = model(input_ids=ids, labels=ids).loss
        loss.backward()
        optimizer.step()
        scheduler.step()
        optimizer.zero_grad()
        if step % 10 == 0:
            losses.append({"step": step, "loss": round(loss.item(), 4)})
            print("step %d loss %.4f" % (step, loss.item()), flush=True)
        if step % a.save_every == 0:
            save(a.out, step, a.method, model, tokenizer, optimizer, scheduler, losses)


if __name__ == "__main__":
    main()
