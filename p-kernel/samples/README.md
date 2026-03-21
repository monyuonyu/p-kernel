# p-kernel サンプル集

p-kernel の **ring-3 ユーザー空間プログラム**のサンプルコード集です。
`INT 0x80` システムコールを使って、カーネルの機能を実際に動かしながら学べます。

このディレクトリのソースコードは**アーキテクチャ非依存**です。
アーキテクチャ固有のビルドインフラ（`plibc.h`、リンカスクリプト）は
`userland/<arch>/` に分離されています。

```
p-kernel/
├── samples/              ← このディレクトリ（アーキ非依存ソース）
│   ├── 01_hello/
│   ├── 02_posix_io/
│   ├── 03_rtos_task/
│   ├── 04_rtos_sync/
│   ├── 05_all_demo/
│   ├── 06_net_infer/
│   ├── 07_stdin_echo/
│   ├── 08_http_get/
│   └── 09_readdir/
│
└── userland/
    ├── x86/              ← x86 用ビルドインフラ (INT 0x80 / ELF32)
    │   ├── plibc.h
    │   ├── user.ld
    │   └── Makefile
    └── arm/              ← (将来) ARM 用ビルドインフラ (SVC / ELF32)
```

---

## サンプルの概要

### 01_hello — Hello World
最もシンプルなサンプルです。
`plibc.h` を使ってシリアルポートへ文字列を出力します。
→ まずここから始めてください。

### 02_posix_io — POSIX ファイル I/O
POSIX 互換の `open` / `write` / `read` / `lseek` / `close` を使い、
FAT32 ファイルシステムへのファイル読み書きを行います。

### 03_rtos_task — RTOS タスク管理
T-Kernel ネイティブ API でタスクを作成・起動します。
優先度スケジューリング（FIFO）とタイムスライス（RR）の両方を実演します。

### 04_rtos_sync — 同期プリミティブ
セマフォとイベントフラグを使ったタスク間同期を実演します。
複数タスクが協調して動作するパターンを学べます。

### 05_all_demo — 総合デモ
上記サンプルの全機能を網羅した自動確認プログラムです。
OK=59 NG=0 を確認済み（QEMU x86_64）。

### 06_net_infer — AI 推論 + UDP ネットワーク配信
センサーデータを AI で分類し、結果を UDP で配信するサンプルです。
カーネル内蔵の MLP ニューラルネットワーク (`SYS_INFER`) と
非同期 AI ジョブ (`SYS_AI_SUBMIT` / `SYS_AI_WAIT`)、
UDP 送受信 (`SYS_UDP_BIND` / `SYS_UDP_SEND` / `SYS_UDP_RECV`) を使います。

### 07_stdin_echo — 標準入力エコー
`sys_read(fd=0)` で標準入力から 1 行読み込み、そのままエコーするサンプルです。
空行を入力すると終了します。
→ stdin syscall の動作を最もシンプルに確認できます。

### 08_http_get — HTTP GET (TCP クライアント)
`sys_tcp_connect` / `sys_tcp_write` / `sys_tcp_read` / `sys_tcp_close` を使って
HTTP/1.0 GET リクエストを 10.0.2.2:80 に送るサンプルです。
→ TCP syscall の一連の流れを学べます。`make run-disk-net` でネットワーク付き起動が必要です。

### 09_readdir — ディレクトリ一覧
`sys_readdir(path, buf, max)` でディレクトリのエントリ一覧を取得するサンプルです。
ファイル名・サイズ・種別（ファイル/ディレクトリ）を表示します。
→ ファイルシステム操作の仕上げとして試してください。

---

## ビルド方法 (x86)

```bash
# x86 向けに全 ELF をビルド
cd userland/x86
make

# 個別ビルド
make 01_hello/hello.elf
make 05_all_demo/all_demo.elf
make 06_net_infer/net_infer.elf
make 07_stdin_echo/stdin_echo.elf
make 08_http_get/http_get.elf
make 09_readdir/readdir.elf
```

**必要なツール:**
- `i686-linux-gnu-gcc` (32 ビットクロスコンパイラ)
- `i686-linux-gnu-ld`

## QEMU での実行方法 (x86)

```bash
# カーネルイメージとディスクイメージのビルド
cd boot/x86
make disk          # disk.img を作成（全 ELF を FAT32 ディスクに格納）
make run-disk      # QEMU 起動

# シェルコマンドで実行
p-kernel> exec hello.elf
p-kernel> exec posix_io.elf
p-kernel> exec rtos_task.elf
p-kernel> exec rtos_sync.elf
p-kernel> exec all_demo.elf
p-kernel> exec net_infer.elf
p-kernel> exec stdin_echo.elf
p-kernel> exec http_get.elf
p-kernel> exec readdir.elf
```

---

## plibc.h について

`plibc.h` は p-kernel 専用のユーザー空間ライブラリです。
libc を一切使わずにシステムコールを呼び出します。
x86 版は `userland/x86/plibc.h` にあり、`INT 0x80` を使います。

```
POSIX 互換 API:
  sys_write, sys_read, sys_open, sys_close,
  sys_lseek, sys_mkdir, sys_unlink, sys_rename, sys_exit

T-Kernel ネイティブ API (0x100+):
  タスク管理:   tk_cre_tsk, tk_sta_tsk, tk_ext_tsk,
                tk_slp_tsk, tk_wup_tsk, tk_chg_pri,
                tk_chg_slt, tk_ref_tsk, tk_del_tsk
  セマフォ:     tk_cre_sem, tk_del_sem, tk_sig_sem, tk_wai_sem
  イベントフラグ: tk_cre_flg, tk_del_flg, tk_set_flg,
                  tk_clr_flg, tk_wai_flg

ネットワーク API (0x200+):
  sys_udp_bind, sys_udp_send, sys_udp_recv
  sys_tcp_connect, sys_tcp_write, sys_tcp_read, sys_tcp_close

AI 推論 API (0x210+):
  sys_infer, sys_ai_submit, sys_ai_wait

文字列ユーティリティ:
  plib_strlen, plib_puts, plib_puti, plib_putu, plib_put_ip
```

---

## システムコール番号一覧

| 番号   | 名前             | 説明                        |
|--------|------------------|-----------------------------|
| 1      | SYS_EXIT         | プロセス終了                |
| 3      | SYS_READ         | ファイル読み込み            |
| 4      | SYS_WRITE        | ファイル書き込み（fd=1でシリアル出力） |
| 5      | SYS_OPEN         | ファイルオープン            |
| 6      | SYS_CLOSE        | ファイルクローズ            |
| 7      | SYS_LSEEK        | ファイルシーク              |
| 8      | SYS_MKDIR        | ディレクトリ作成            |
| 9      | SYS_UNLINK       | ファイル削除                |
| 10     | SYS_RENAME       | ファイル名変更              |
| 0x100  | SYS_TK_CRE_TSK   | タスク作成                  |
| 0x101  | SYS_TK_STA_TSK   | タスク起動                  |
| 0x102  | SYS_TK_EXT_TSK   | タスク終了（自タスク）      |
| 0x103  | SYS_TK_SLP_TSK   | タスクスリープ              |
| 0x104  | SYS_TK_WUP_TSK   | タスク起床                  |
| 0x105  | SYS_TK_CHG_PRI   | 優先度変更                  |
| 0x106  | SYS_TK_CHG_SLT   | タイムスライス変更          |
| 0x107  | SYS_TK_REF_TSK   | タスク情報参照              |
| 0x108  | SYS_TK_DEL_TSK   | タスク削除（DORMANT 状態）  |
| 0x110  | SYS_TK_CRE_SEM   | セマフォ作成                |
| 0x111  | SYS_TK_DEL_SEM   | セマフォ削除                |
| 0x112  | SYS_TK_SIG_SEM   | セマフォ送信                |
| 0x113  | SYS_TK_WAI_SEM   | セマフォ待ち                |
| 0x120  | SYS_TK_CRE_FLG   | イベントフラグ作成          |
| 0x121  | SYS_TK_DEL_FLG   | イベントフラグ削除          |
| 0x122  | SYS_TK_SET_FLG   | フラグビット セット          |
| 0x123  | SYS_TK_CLR_FLG   | フラグビット クリア          |
| 0x124  | SYS_TK_WAI_FLG   | フラグ待ち                  |
| 0x200  | SYS_UDP_BIND     | UDP ポートバインド          |
| 0x201  | SYS_UDP_SEND     | UDP 送信                    |
| 0x202  | SYS_UDP_RECV     | UDP 受信（タイムアウト付き）|
| 0x203  | SYS_TCP_CONNECT  | TCP 接続確立                |
| 0x204  | SYS_TCP_WRITE    | TCP 送信                    |
| 0x205  | SYS_TCP_READ     | TCP 受信（タイムアウト付き）|
| 0x206  | SYS_TCP_CLOSE    | TCP クローズ＋解放          |
| 11     | SYS_READDIR      | ディレクトリ一覧取得        |
| 0x210  | SYS_INFER        | MLP 推論（同期）            |
| 0x211  | SYS_AI_SUBMIT    | AI ジョブ投入（非同期）     |
| 0x212  | SYS_AI_WAIT      | AI ジョブ完了待ち           |
