# p-kernel ユーザースペース サンプル集

p-kernel の **ring-3 ユーザー空間プログラム**の開発ガイドです。
`INT 0x80` システムコールを使って、カーネルの機能を実際に動かしながら学べます。

---

## フォルダ構成

```
user_hello/
├── plibc.h           ユーザー空間ライブラリ（システムコールラッパー）
├── user.ld           リンカスクリプト（ELF ロードアドレス定義）
│
├── 01_hello/         ステップ1: Hello World — 最初の一歩
├── 02_posix_io/      ステップ2: POSIX ファイル I/O
├── 03_rtos_task/     ステップ3: RTOS タスク管理
├── 04_rtos_sync/     ステップ4: 同期プリミティブ（セマフォ・イベントフラグ）
└── test_all/         テストスイート（全機能の動作検証）
```

---

## ビルド方法

```bash
# サンプル全体をビルド
cd boot/x86/user_hello
make

# 個別にビルド
make 01_hello/hello.elf
make 02_posix_io/posix_io.elf
make 03_rtos_task/rtos_task.elf
make 04_rtos_sync/rtos_sync.elf
make test_all/test_all.elf
```

**必要なツール:**
- `i686-linux-gnu-gcc` (32ビットクロスコンパイラ)
- `i686-linux-gnu-ld`

---

## QEMU での実行方法

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
p-kernel> exec test_all.elf
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

### test_all — テストスイート
上記サンプルの全機能を網羅した自動テストです。
PASS=59 FAIL=0 を確認済み（QEMU x86_64）。

---

## plibc.h について

`plibc.h` は p-kernel 専用のユーザー空間ライブラリです。
libc を一切使わずに `INT 0x80` シスコールを呼び出します。

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

文字列ユーティリティ:
  plib_strlen, plib_puts, plib_puti, plib_putu
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
