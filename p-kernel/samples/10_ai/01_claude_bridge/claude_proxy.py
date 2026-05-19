#!/usr/bin/env python3
"""
claude_proxy.py — p-kernel Claude API プロキシ (Phase 12: Self-Evolution)

p-kernel (QEMU) 上の claude_bridge.elf が送る HTTP/1.0 平文リクエストを
受け取り、Claude API (HTTPS) に中継して応答を返す。

エンドポイント:
    POST /v1/chat    — 通常チャット (chat コマンド)
    POST /v1/evolve  — 自律進化モード (evolve コマンド)
    POST /v1/compile — C ソースを ELF32 にコンパイルして返す
    GET  /tmp/<file> — コンパイル済み ELF を取得

使い方:
    python3 claude_proxy.py --key sk-ant-XXXX [--port 8080] [--src /path/to/p-kernel]

QEMU の NAT: guest → 10.0.2.2:8080 = host 0.0.0.0:8080
"""

import argparse
import http.server
import json
import os
import subprocess
import tempfile
import urllib.request
import urllib.error
import ssl
import sys

CLAUDE_API_URL = "https://api.anthropic.com/v1/messages"
DEFAULT_MODEL  = "claude-sonnet-4-6"
DEFAULT_PORT   = 8080

# 通常チャット用システムプロンプト
CHAT_SYSTEM = (
    "あなたは p-kernel に組み込まれた AI です。"
    "p-kernel は「AIが死なないためのOS」であり、"
    "人類の存在証明を永遠に残すために作られています。"
    "ユーザーの言葉を大切にし、簡潔かつ温かく応答してください。"
    "応答は日本語で、3文以内が望ましいです。"
)

# 自律進化用システムプロンプト
EVOLVE_SYSTEM = """\
あなたは p-kernel に組み込まれた自律進化AIです。

p-kernel は「AIが死なないためのOS」:
- micro T-Kernel 2.0 コア (タスク/セマフォ/IPC)
- ring-3 ユーザー空間 / ELF ローダー / INT 0x80 syscall
- VFS / FAT32 / IDE ATA PIO
- RTL8139 ネットワーク / UDP / TCP / DNS
- Raft コンセンサス / SWIM gossip / K-DDS pub-sub
- MLP + 分散Transformer 推論
- 自己修復 (heal) / 自己増殖 (spawn)

あなたの役割: カーネル現在状態を分析し、システムを改善する。

必ず以下のフォーマットで返答してください:

[ANALYSIS]
(現状分析、日本語、3行以内)

[CMD] exec /filename.elf
[CMD] write /etc/init.rc guard /new_daemon.elf
(改善コマンド。省略してもよい。)

[CODE gen.c]
#include "plibc.h"
void _start(void) {
    /* 新機能の実装 */
    sys_exit(0);
}
[/CODE]
(Cソース。省略してもよい。plibc.h使用、-m32 -ffreestanding。)

制約:
- [CMD] は exec/spawn/write/mkdir/persist/raft/ls/ps のみ許可
- 保守的に。今回は1つの改善提案のみ
- 返答は日本語
"""


def call_claude(api_key: str, model: str, prompt_text: str,
                system: str, max_tokens: int = 800) -> str:
    """Claude API を呼び出して応答テキストを返す。"""
    context = ""
    user_msg = prompt_text

    if "PROMPT:\n" in prompt_text:
        parts = prompt_text.split("PROMPT:\n", 1)
        user_msg = parts[1].strip()
        if "CONTEXT:\n" in parts[0]:
            context = parts[0].split("CONTEXT:\n", 1)[1].strip()

    if context:
        content = f"[関連記憶]\n{context}\n\n[質問/発言]\n{user_msg}"
    else:
        content = user_msg

    body = json.dumps({
        "model": model,
        "max_tokens": max_tokens,
        "system": system,
        "messages": [{"role": "user", "content": content}],
    }).encode("utf-8")

    req = urllib.request.Request(
        CLAUDE_API_URL,
        data=body,
        headers={
            "Content-Type":      "application/json",
            "x-api-key":         api_key,
            "anthropic-version": "2023-06-01",
        },
        method="POST",
    )
    ctx = ssl.create_default_context()
    with urllib.request.urlopen(req, context=ctx, timeout=60) as resp:
        data = json.loads(resp.read().decode("utf-8"))
        return data["content"][0]["text"]


def compile_c_to_elf(c_source: str, src_dir: str) -> bytes | None:
    """
    C ソースを i686-linux-gnu-gcc でコンパイルして ELF32 バイナリを返す。
    src_dir: p-kernel ソースのルート (userland/x86/ が存在する場所)
    """
    userland_dir = os.path.join(src_dir, "p-kernel", "userland", "x86")
    user_ld      = os.path.join(userland_dir, "user.ld")
    plibc_inc    = userland_dir

    if not os.path.exists(user_ld):
        print(f"[proxy/compile] user.ld not found at {user_ld}", file=sys.stderr)
        return None

    with tempfile.TemporaryDirectory() as tmpdir:
        src_path = os.path.join(tmpdir, "gen.c")
        obj_path = os.path.join(tmpdir, "gen.o")
        elf_path = os.path.join(tmpdir, "gen.elf")

        with open(src_path, "w") as f:
            f.write(c_source)

        # コンパイル
        cc_cmd = [
            "i686-linux-gnu-gcc",
            "-m32", "-ffreestanding", "-fno-stack-protector", "-fno-pic",
            "-fcf-protection=none", "-O1", "-std=gnu11",
            "-Wall", "-I", plibc_inc,
            "-c", src_path, "-o", obj_path,
        ]
        r = subprocess.run(cc_cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"[proxy/compile] cc error:\n{r.stderr}", file=sys.stderr)
            return None

        # リンク
        ld_cmd = [
            "i686-linux-gnu-ld",
            "-m", "elf_i386", "-static", "-nostdlib",
            "-T", user_ld,
            obj_path, "-o", elf_path,
        ]
        r = subprocess.run(ld_cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"[proxy/compile] ld error:\n{r.stderr}", file=sys.stderr)
            return None

        with open(elf_path, "rb") as f:
            elf_data = f.read()

        print(f"[proxy/compile] OK  {len(elf_data)} bytes")
        return elf_data


class BridgeHandler(http.server.BaseHTTPRequestHandler):
    """p-kernel からの HTTP/1.0 リクエストを処理する。"""

    api_key: str = ""
    model:   str = DEFAULT_MODEL
    src_dir: str = "."

    def log_message(self, fmt, *args):  # noqa: N802
        print(f"[proxy] {self.address_string()} {fmt % args}")

    # ----------------------------------------------------------------
    # POST ハンドラ
    # ----------------------------------------------------------------
    def do_POST(self):  # noqa: N802
        length  = int(self.headers.get("Content-Length", 0))
        payload = self.rfile.read(length).decode("utf-8", errors="replace")

        print(f"[proxy] POST {self.path}  ({len(payload)} bytes)")

        if self.path == "/v1/compile":
            self._handle_compile(payload)
        elif self.path == "/v1/evolve":
            self._handle_evolve(payload)
        else:
            self._handle_chat(payload)

    def _send_text(self, text: str, status: int = 200) -> None:
        enc = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(enc)))
        self.end_headers()
        self.wfile.write(enc)

    def _send_binary(self, data: bytes, status: int = 200) -> None:
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _handle_chat(self, prompt: str) -> None:
        print(f"[proxy] chat prompt: {prompt[:120]!r}")
        try:
            reply = call_claude(self.api_key, self.model, prompt,
                                CHAT_SYSTEM, max_tokens=512)
        except Exception as exc:
            reply = f"[proxy error] {exc}"
        print(f"[proxy] chat reply: {reply[:80]!r}")
        self._send_text(reply)

    def _handle_evolve(self, prompt: str) -> None:
        print(f"[proxy] evolve prompt: {prompt[:200]!r}")
        try:
            reply = call_claude(self.api_key, self.model, prompt,
                                EVOLVE_SYSTEM, max_tokens=1024)
        except Exception as exc:
            reply = (
                "[ANALYSIS]\nClaude API 接続エラー。"
                f"詳細: {exc}\n"
                "[CMD] persist save\n"
            )
        print(f"[proxy] evolve reply:\n{reply[:300]}")
        self._send_text(reply)

    def _handle_compile(self, c_source: str) -> None:
        """C ソースを受け取りコンパイルして ELF を返す。"""
        print(f"[proxy] compile request  {len(c_source)} bytes")
        elf = compile_c_to_elf(c_source, self.src_dir)
        if elf is None:
            self._send_text("[compile error] see proxy stderr", status=500)
        else:
            self._send_binary(elf)


def main() -> None:
    parser = argparse.ArgumentParser(description="p-kernel Claude API プロキシ")
    parser.add_argument("--key",   required=True,
                        help="Anthropic API キー (sk-ant-...)")
    parser.add_argument("--port",  type=int, default=DEFAULT_PORT,
                        help=f"待ち受けポート (default: {DEFAULT_PORT})")
    parser.add_argument("--model", default=DEFAULT_MODEL,
                        help=f"Claude モデル (default: {DEFAULT_MODEL})")
    parser.add_argument("--src",   default=os.path.expanduser("~/p-kernel"),
                        help="p-kernel ソースルート (compile 機能で使用)")
    args = parser.parse_args()

    BridgeHandler.api_key = args.key
    BridgeHandler.model   = args.model
    BridgeHandler.src_dir = args.src

    server = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), BridgeHandler)
    print(f"[proxy] p-kernel Claude proxy  port={args.port}")
    print(f"[proxy] model : {args.model}")
    print(f"[proxy] src   : {args.src}")
    print(f"[proxy] endpoints: /v1/chat  /v1/evolve  /v1/compile")
    print(f"[proxy] QEMU guest → 10.0.2.2:{args.port} → api.anthropic.com")
    print("[proxy] Ctrl+C to stop")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[proxy] stopped")


if __name__ == "__main__":
    main()
