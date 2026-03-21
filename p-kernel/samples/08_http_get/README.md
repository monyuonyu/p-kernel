# ステップ8: HTTP GET (TCP クライアント)

TCP 接続を確立し、HTTP GET リクエストを送信してレスポンスを表示するサンプルです。

## 学べること

- **`sys_tcp_connect(ip, port, tmo)`** — TCP 3-way handshake で接続を確立する
- **`sys_tcp_write(handle, buf, len)`** — 確立した接続へデータを送信する
- **`sys_tcp_read(handle, buf, len, tmo)`** — データを受信する（タイムアウト付き）
- **`sys_tcp_close(handle)`** — FIN を送り接続をクローズする

## ビルドと実行

```bash
cd userland/x86
make 08_http_get/http_get.elf

cd boot/x86
make disk && make run-disk   # ネットワーク付き (RTL8139 QEMU user-net)
p-kernel> exec http_get.elf
```

## 期待する出力

### ネットワーク未接続 / ホスト HTTP サーバーなし
```
========================================
 http_get: TCP + HTTP GET デモ
========================================

  tcp_connect(10.0.2.2:80): failed (タイムアウトまたはサーバー未起動)

  ※ QEMU ホスト (10.0.2.2) で HTTP サーバーを
    起動すると受信できます。

========================================
 http_get: done
========================================
[proc] exited (code=0)
```

### ホストで HTTP サーバーを起動した場合

```bash
# ホスト側でシンプルな HTTP サーバーを起動
python3 -m http.server 80
```

```
  tcp_connect(10.0.2.2:80): OK (handle=0)

  tcp_write: sending HTTP GET ...
  tcp_write: 47 bytes sent

--- HTTP response ---
HTTP/1.0 200 OK
...
--- end of response ---

  total: 512 bytes received
  tcp_close: OK

========================================
 http_get: done
========================================
[proc] exited (code=0)
```

## TCP syscall 一覧

| syscall | 説明 |
|---------|------|
| `sys_tcp_connect(ip, port, tmo)` | TCP 接続確立。ハンドル (0-3) を返す。 |
| `sys_tcp_write(h, buf, len)` | データ送信。送信バイト数を返す。 |
| `sys_tcp_read(h, buf, len, tmo)` | データ受信。受信バイト数または 0 (切断)。 |
| `sys_tcp_close(h)` | 接続クローズ＋解放。 |
