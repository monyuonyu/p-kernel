# ステップ5: 総合デモ (all_demo)

p-kernel の全機能を自動検証する包括的なデモスイートです。
**OK=59 NG=0** を確認済み（QEMU x86_64）。

## テスト一覧

| テスト | カテゴリ | 内容 |
|--------|----------|------|
| D1     | POSIX I/O | open / write / read / close の基本動作 |
| D2     | POSIX I/O | lseek — SEEK_SET / SEEK_END |
| D3     | POSIX I/O | mkdir / rename / unlink |
| D4     | セマフォ | 基本 signal / wait |
| D5     | セマフォ | バルク signal / wait (cnt > 1) |
| D6     | セマフォ | TMO_POL (即時ポーリング) |
| D7     | フラグ | TWF_ANDW (AND 待ち) |
| D8     | フラグ | TWF_ORW (OR 待ち) |
| D9     | フラグ | TMO_POL + tk_clr_flg |
| D10    | タスク | tk_ref_tsk — 情報参照 |
| D11    | タスク | tk_chg_pri — 優先度変更 |
| D12    | タスク | tk_chg_slt — スライス変更 |
| D13    | タスク | tk_wup_tsk — 早期起床 |
| D14    | タスク | tk_slp_tsk — 自然タイムアウト (E_TMOUT) |
| D15    | スケジューリング | FIFO 優先度抢占 |
| D16    | スケジューリング | RR 同優先ローテーション |
| D17    | スケジューリング | FIFO + RR 混在 |
| D18    | スケジューリング | タスクリソース再利用 (リークなし) |
| D19    | スケジューリング | タスク上限 (USR_TASK_MAX=8) |
| D20    | POSIX I/O | 部分読み (lseek 後の指定バイト読み) |

## ソースファイル構成

```
05_all_demo/
├── common.h   ASSERT マクロ・グローバル宣言・関数プロトタイプ
├── main.c     _start()・グローバル変数定義・ヘルパー関数
├── posix.c    T1, T2, T3, T20
├── sem.c      T4, T5, T6
├── flg.c      T7, T8, T9
├── task.c     T10, T11, T12, T13, T14
└── sched.c    T15, T16, T17, T18, T19
```

## ビルドと実行

```bash
# ビルド
cd boot/x86/user_hello
make 05_all_demo/all_demo.elf

# QEMU で実行
p-kernel> exec all_demo.elf
```

## 期待する出力

```
============================================
  p-kernel デモスイート (all_demo.elf)
============================================

>>> POSIX ファイル I/O テスト
--- T1: POSIX 基本ファイルI/O ---
[ OK] T1-open
[ OK] T1-write
...
>>> セマフォ テスト
...
============================================
  結果: PASS=59  FAIL=0
  全テスト合格!
============================================
```

## テストの設計方針

### タイムアウト安全設計
`tk_wai_sem` / `tk_wai_flg` には必ず **5000ms** の有限タイムアウトを使います。
`TMO_FEVR` は使いません。タスク作成に失敗した場合にデッドロックになるためです。

### リソース管理
タスクを作成したら必ず `tk_del_tsk(tid)` で TCB を解放します。
セマフォ・イベントフラグも必ず `tk_del_sem` / `tk_del_flg` で削除します。

### 冪等性 (繰り返し実行できる)
T3 (mkdir) は FAT32 の残骸をクリーンアップしてから実行するため、
2 回目以降も FAIL しません。

## テスト失敗時の確認ポイント

| 症状 | 原因の候補 |
|------|------------|
| T6/T9 が `got=-50 expect=?` | E_TMOUT の値が期待値と違う |
| D15 の順序が逆 | FIFO 抢占が機能していない |
| D19 が FAIL | USR_TASK_MAX が変更された |
| テストが止まる | TMO_FEVR を使っている箇所でデッドロック |
