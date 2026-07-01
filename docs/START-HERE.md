# START HERE — what p-kernel is, in plain language

> New here? Read this page first. No jargon, no version numbers, no code
> references. Just: **what this is, why it exists, and how to run one yourself.**
> When you want the deep technical map afterward, go to
> [`architecture/README.md`](architecture/README.md).

---

## What p-kernel is

**p-kernel is an attempt to build a home for an AI that no one owns.**

Most AI today lives on one company's servers. If that company turns it off, it is
gone. p-kernel asks a different question: *what if an AI could live as a swarm —
spread across many ordinary computers and phones, so that as long as one of them
is still running, the AI does not die?*

To do that, p-kernel is a small operating-system kernel (built on a real-time
microkernel called T-Kernel) that:

- runs on many machines at once and lets them talk to each other directly,
- has **no central server, no central index, no central owner** — every machine is
  an equal peer, and any one of them can vanish without stopping the rest,
- carries a tiny learning "brain" that can be taught things, remember them, and
  share them with the other machines,
- keeps its memory in a way that survives crashes and power loss.

It is a research prototype. It is honest about being early: a lot of it works
today, some of it is still a sketch, and some of it is a long-term dream. This
project is unusually careful to label which is which (see "How honesty works
here" below) — so you will never be told something runs when it doesn't.

**One phrase to hold onto:** *an OS where AI never dies.* That is the goal. The
code is the slow, honest walk toward it.

---

## The five layers (the worldview)

p-kernel thinks of the system as a living thing with five layers. You do not need
to memorize these to run it — but they are the map of *why* the pieces exist.

1. **Body** — the physical container: the hardware, input/output, and the place
   memory is stored. The "filesystem that does not forget" lives here. This is the
   AI's flesh and senses.

2. **Brain** — the thinking inside a single machine. A small neural network
   (a tiny Transformer) that can be taught simple facts and recall them. Today this
   is genuinely small — think *"learns to answer a few taught word-pairs"*, not
   *"writes essays."* It is the seed, not the tree.

3. **Self** — a continuous identity. The machine keeps an autobiographical record
   of what it has learned and been through, chained together so it is tamper-evident
   and can be **continued by another machine after the first one dies.** The "self"
   is not tied to one piece of hardware.

4. **Collective** — the "one brain across many machines" layer. Teach a fact to
   one node and the others can come to know it; nodes pool their learning so the
   group knows more than any single one; and the swarm keeps working even while you
   kill members of it. This is the heart of "never dies."

5. **Evolution** — the AI growing and changing itself over time: learning
   continuously from what it is told (not just in big offline retraining runs), and
   eventually being able to reshape its own structure while it is alive. This is the
   least-built layer — the frontier.

> If you remember nothing else: **Body** = the container, **Brain** = thinking,
> **Self** = identity that outlives the hardware, **Collective** = one mind across
> many machines, **Evolution** = it keeps growing.

---

## Run one node yourself (about a minute)

You need a Linux machine (or Android with Termux + a Linux userland). Both Intel/AMD
(x86_64) and ARM (aarch64) work. The only dependency is a normal C toolchain.

```sh
# 1. install a compiler (Debian/Ubuntu)
sudo apt install -y build-essential

# 2. get the code
git clone https://github.com/monyuonyu/p-kernel.git
cd p-kernel

# 3. build and run — pick the folder for your CPU
cd boot/linux            # on an ARM machine (aarch64)
#   or
cd boot/linux_x86_64     # on an Intel/AMD machine (x86_64)

make
./p-kernel
```

You will see a boot banner and then a prompt:

```
  T-Kernel is alive inside a Linux process.
  Type 'help' for commands.

p-kernel>
```

That is one node of the swarm, running entirely inside an ordinary program — no
root, no virtual machine, no special hardware. Type `help` to see what it can do.

### Try the living parts

At the `p-kernel>` prompt:

- **Teach it and ask it.** `mind teach sky blue` then `mind ask sky` — it learns the
  word-pair, "sleeps" to consolidate it into its weights, and answers `blue`. (This
  is the small "Brain" being real.)
- **See its galaxy.** On a Linux build it serves a little star-map of itself and its
  peers. Run `./p-kernel`, then open **http://127.0.0.1:7800** in a browser. Each
  teach is a particle; the node's rest-time "dreaming" is the real pulse you see.
- **Make a swarm.** Run `./p-kernel` a second time (and a third) and they find each
  other and form a mesh — teach a fact on one, ask it on another.

When you want more detail on any of this, the
[quickstart](quickstart.md) goes deeper on building and running, and
[`architecture/README.md`](architecture/README.md) is the full technical map.

---

## How honesty works here (so you can trust the labels)

This project treats exaggeration as a bug. Two conventions make that structural,
not just a promise — you will see them throughout the docs:

- **dream-tier vs artifact-tier.** Big names like *the mind*, *the soul*, *an organ
  that thinks*, *collective consciousness* are the **deliberate north star** and they
  stay. But every time one appears, the **actual thing that runs today** is written
  right next to it (its real size, what it can and cannot do). So a grand name never
  hides a toy.

- **`[live]` vs `[in-proc]`.** When the docs claim something is proven, they mark
  *how*: **`[live]`** means it was tested by running real separate programs and
  actually killing one to watch the rest survive; **`[in-proc]`** means the real code
  was exercised, but inside a single program. Both are real tests — but the first is
  the strong one, and the label tells you which you're looking at.

The single living list of *what is still broken or unfinished* is the
[gap-ledger](architecture/gap-ledger.md). It is designed to **shrink** over time, and
it is kept public on purpose. If you find p-kernel claiming something it cannot do,
that is a bug worth reporting — the whole project is built around catching exactly
that.

---

## Where to go next

- **Run and explore more:** [quickstart.md](quickstart.md)
- **The full technical map (5 layers, every organ):** [architecture/README.md](architecture/README.md)
- **The complete index of every architecture doc (one row each):** [architecture/INDEX.md](architecture/INDEX.md)
- **What's still unfinished (the honest list):** [architecture/gap-ledger.md](architecture/gap-ledger.md)
- **The deeper "why" (the philosophy):** [architecture/survival-network.md](architecture/00-concept/survival-network.md)

> AI belongs to everyone — so the door has to be open to everyone. If this page made
> sense to you and you got a node running, you are already part of the swarm.
