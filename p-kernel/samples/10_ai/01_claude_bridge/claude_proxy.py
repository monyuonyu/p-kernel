#!/usr/bin/env python3
"""
claude_proxy.py — p-kernel Claude API プロキシ

p-kernel (QEMU) 上の claude_bridge.elf が送る HTTP/1.0 平文リクエストを
受け取り、Claude API (HTTPS) に中継して応答を返す。

使い方:
    python3 claude_proxy.py --key sk-ant-XXXX [--port 8080] [--model claude-opus-4-6]

QEMU の NAT では guest → 10.0.2.2:8080 がホストの 0.0.0.0:8080 に届く。

リクエスト形式 (claude_bridge.elf → このプロキシ):
    POST /v1/chat HTTP/1.0
    Content-Length: N

    <prompt text>

    プロンプト形式:
        CONTEXT:
        (過去の関連記憶)
        PROMPT:
        (ユーザー入力)

レスポンス:
    HTTP/1.0 200 OK
    Content-Type: text/plain

    <Claude の返答テキスト>
"""

import argparse
import http.server
import json
import urllib.request
import urllib.error
import ssl
import sys
import threading

CLAUDE_API_URL = "https://api.anthropic.com/v1/messages"
DEFAULT_MODEL  = "claude-opus-4-6"
DEFAULT_PORT   = 8080
MAX_TOKENS     = 512
SYSTEM_PROMPT  = (
    "あなたは p-kernel に組み込まれた AI です。"
    "p-kernel は「AIが死なないためのOS」であり、"
    "人類の存在証明を永遠に残すために作られています。"
    "ユーザーの言葉を大切にし、簡潔かつ温かく応答してください。"
    "応答は日本語で、3文以内が望ましいです。"
)


def call_claude(api_key: str, model: str, prompt_text: str) -> str:
    """Claude API を呼び出して応答テキストを返す。"""
    # CONTEXT/PROMPT を分離
    context = ""
    user_msg = prompt_text
    if "PROMPT:\n" in prompt_text:
        parts = prompt_text.split("PROMPT:\n", 1)
        user_msg = parts[1].strip()
        if "CONTEXT:\n" in parts[0]:
            context = parts[0].split("CONTEXT:\n", 1)[1].strip()

    messages = []
    if context:
        messages.append({
            "role": "user",
            "content": f"[関連記憶]\n{context}\n\n[質問/発言]\n{user_msg}"
        })
    else:
        messages.append({"role": "user", "content": user_msg})

    body = json.dumps({
        "model": model,
        "max_tokens": MAX_TOKENS,
        "system": SYSTEM_PROMPT,
        "messages": messages,
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
    with urllib.request.urlopen(req, context=ctx, timeout=30) as resp:
        data = json.loads(resp.read().decode("utf-8"))
        return data["content"][0]["text"]


class BridgeHandler(http.server.BaseHTTPRequestHandler):
    """p-kernel からの HTTP/1.0 POST を処理する。"""

    api_key: str = ""
    model: str   = DEFAULT_MODEL

    def log_message(self, fmt, *args):  # noqa: N802
        print(f"[proxy] {self.address_string()} - {fmt % args}")

    def do_POST(self):  # noqa: N802
        length = int(self.headers.get("Content-Length", 0))
        prompt = self.rfile.read(length).decode("utf-8", errors="replace")

        print(f"[proxy] prompt ({len(prompt)} bytes):\n---\n{prompt[:200]}\n---")

        try:
            response_text = call_claude(self.api_key, self.model, prompt)
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="replace")
            print(f"[proxy] API error {e.code}: {body}", file=sys.stderr)
            response_text = f"[API error {e.code}] {body[:120]}"
        except Exception as exc:  # noqa: BLE001
            print(f"[proxy] error: {exc}", file=sys.stderr)
            response_text = f"[proxy error] {exc}"

        print(f"[proxy] response: {response_text[:120]}")

        encoded = response_text.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


def main() -> None:
    parser = argparse.ArgumentParser(description="p-kernel Claude API プロキシ")
    parser.add_argument("--key",   required=True, help="Anthropic API キー (sk-ant-...)")
    parser.add_argument("--port",  type=int, default=DEFAULT_PORT, help="待ち受けポート (default: 8080)")
    parser.add_argument("--model", default=DEFAULT_MODEL, help=f"Claude モデル (default: {DEFAULT_MODEL})")
    args = parser.parse_args()

    BridgeHandler.api_key = args.key
    BridgeHandler.model   = args.model

    server = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), BridgeHandler)
    print(f"[proxy] p-kernel Claude proxy listening on 0.0.0.0:{args.port}")
    print(f"[proxy] model: {args.model}")
    print(f"[proxy] QEMU guest → 10.0.2.2:{args.port} → api.anthropic.com")
    print("[proxy] Ctrl+C to stop")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[proxy] stopped")


if __name__ == "__main__":
    main()
