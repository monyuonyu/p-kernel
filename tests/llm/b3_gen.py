#!/usr/bin/env python3
"""b3_gen.py - make the B3 texts with the teacher (ROADMAP B3; not in CI).

The way of making was fixed before any text was made, in
docs/architecture/30-module/research/b3-data.md section 2:

  teacher   SmolLM2-135M base model, the same Q8_0 GGUF the kernel reads,
            through PyTorch/transformers in batches (the kernel's own forward
            is about 10 B/s, this about 550 B/s)
  prompt    an opening sentence from one of 12 patterns with a noun, a verb
            and an adjective drawn at random (TinyStories, arXiv 2305.07759
            section 2); the draw is seeded by the text number
  sampling  temperature 0.8, top-k 40, repetition penalty 1.1, <= 256 new tokens
  kept      only the continuation (the opening is not learned: the 12 patterns
            would turn into amount); filtering is b3_filter.py

  b3_gen.py --model-dir D --gguf F --first N --count C --out texts.jsonl
            [--batch 64] [--threads 2]
  b3_gen.py --model-dir D --gguf F --encode text.txt     (TEACHER check:
            prints the first 256 token ids of the text, one per line)
  b3_gen.py --model-dir D --gguf F --top1 ids.txt        (TEACHER check:
            prints "<pos> <top-1 id>" for the ids, one per line)

It needs torch, transformers and gguf (the venv used is written in the log of
the run, research/b3-data-log.md). Output texts are not kept in git; their
sha256 and the command line are.
"""
import json
import random
import sys

NOUNS = ("apple ball bear bed bell bike bird boat book box boy bread bridge brother bug bus cake "
         "candle car cat chair cloud coat cookie cow cup dog doll door dragon drum duck egg "
         "farmer fish flower fox friend frog garden ghost girl goat grandma hat hill horse house "
         "jar key kite king lamp leaf lion map mirror monkey moon mouse mud nest owl paint pan "
         "park pencil penguin pie pig pillow pond puppy queen rabbit rain river robot rock rope "
         "sand school sea seed sheep shell shoe sister snail snow sock spoon star stick stone "
         "sun swing table teacher tent tiger toy train tree truck turtle wagon whale wind window "
         "wolf worm").split()
VERBS = ("bake bounce build carry catch chase clap climb collect cook count cry dance dig draw "
         "dream drop explore fall feed fight find fix float fly fold follow give glow grow help "
         "hide hop hug hunt jump kick knock laugh learn lift listen look lose make mix move "
         "open paint pick plant play pour pull push race read ride roll run save search share "
         "shine shout sing sit skip sleep slide smell smile spin splash stop swim swing taste "
         "teach throw touch trade try visit wait walk wash watch wave whisper win wish write").split()
ADJS = ("angry big bright brave broken busy calm careful cheerful clean clever cold colorful "
        "curious dark dirty dry eager empty fancy fast fluffy friendly funny gentle giant "
        "glad golden good grumpy happy heavy helpful hungry huge kind lazy little lonely loud "
        "lucky magic messy mighty muddy new nice noisy old patient pink polite proud purple "
        "quick quiet rare red rough round sad scared shiny shy silly sleepy slow small smart "
        "smooth soft sparkly special spicy sticky strange strong sunny sweet tall tiny tired "
        "warm wet wild windy wise wooden yellow young").split()

NAMES = ("Lily Tom Ben Mia Sam Anna Max Lucy Leo Emma Jack Zoe Tim Sara Kim Joe").split()

PATTERNS = [
    "Once upon a time, there was a {adj} {noun}. One day it wanted to {verb}.",
    "{name} had a {adj} {noun}. Every morning, {name} liked to {verb} with it.",
    "In a small town, a {adj} {noun} lived near the park. It did not know how to {verb}.",
    "One sunny day, {name} found a {adj} {noun} under a tree and tried to {verb}.",
    "\"Can you help me {verb}?\" asked the {adj} {noun}.",
    "{name} and a {adj} {noun} were best friends. They loved to {verb} together.",
    "The {noun} was very {adj}. Nobody wanted to {verb} with it, until one day",
    "Mom gave {name} a {adj} {noun}. \"Be careful,\" she said, \"and do not {verb} it.\"",
    "There was once a {adj} {noun} who could {verb} better than anyone else.",
    "{name} looked out of the window and saw a {adj} {noun} trying to {verb}.",
    "At school, the teacher showed the class a {adj} {noun}. Then she asked {name} to {verb}.",
    "It was a {adj} night. A little {noun} wanted to {verb}, but it was afraid.",
]


def prompt_of(n):
    """The opening of text number n (a pure function of n)."""
    r = random.Random(1_000_003 * n + 7)
    return PATTERNS[r.randrange(len(PATTERNS))].format(
        noun=r.choice(NOUNS), verb=r.choice(VERBS), adj=r.choice(ADJS), name=r.choice(NAMES))


def load(model_dir, gguf, threads):
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer
    torch.set_num_threads(threads)
    tok = AutoTokenizer.from_pretrained(model_dir, gguf_file=gguf)
    m = AutoModelForCausalLM.from_pretrained(model_dir, gguf_file=gguf, torch_dtype=torch.float32)
    m.eval()
    tok.pad_token = tok.eos_token
    tok.padding_side = "left"
    return torch, tok, m


def generate(a):
    torch, tok, m = load(a["model-dir"], a["gguf"], int(a.get("threads", 2)))
    first, count, bs = int(a["first"]), int(a["count"]), int(a.get("batch", 64))
    with open(a["out"], "w") as out:
        for b0 in range(first, first + count, bs):
            ids = list(range(b0, min(b0 + bs, first + count)))
            x = tok([prompt_of(n) for n in ids], return_tensors="pt", padding=True)
            torch.manual_seed(b0)
            with torch.no_grad():
                y = m.generate(**x, max_new_tokens=256, do_sample=True, temperature=0.8, top_k=40,
                               repetition_penalty=1.1, pad_token_id=tok.eos_token_id)
            gen = y[:, x["input_ids"].shape[1]:]
            for n, g in zip(ids, gen):
                out.write(json.dumps({"id": n, "text": tok.decode(g, skip_special_tokens=True)}) + "\n")
            out.flush()
            print("[b3-gen] texts %d..%d done" % (ids[0], ids[-1]), flush=True)


def top1(a):
    torch, tok, m = load(a["model-dir"], a["gguf"], int(a.get("threads", 2)))
    with open(a["top1"]) as f:
        ids = [int(w) for w in f.read().split()]
    with torch.no_grad():
        logits = m(torch.tensor([ids])).logits[0]
    for p, t in enumerate(logits.argmax(-1).tolist()):
        print("%d %d" % (p, t))


def encode(a):
    from transformers import AutoTokenizer
    tok = AutoTokenizer.from_pretrained(a["model-dir"], gguf_file=a["gguf"])
    with open(a["encode"]) as f:
        ids = tok(f.read())["input_ids"][:256]
    print("\n".join(str(i) for i in ids))


def main(argv):
    a, i = {}, 0
    while i + 1 < len(argv) and argv[i].startswith("--"):
        a[argv[i][2:]] = argv[i + 1]
        i += 2
    if i != len(argv) or "model-dir" not in a or "gguf" not in a:
        print(__doc__)
        return 2
    if "encode" in a:
        encode(a)
    elif "top1" in a:
        top1(a)
    elif all(k in a for k in ("first", "count", "out")):
        generate(a)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
