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
│   ├── 03_stdin_echo/
│   ├── 04_readdir/
│   ├── 05_rtos_task/
│   ├── 06_rtos_sync/
│   ├── 07_mutex/
│   ├── 08_task_ext/
│   ├── 09_mailbox/
│   ├── 10_msgbuf/
│   ├── 11_rendezvous/
│   ├── 12_mempool/
│   ├── 13_time/
│   ├── 14_cyc_alm/
│   ├── 15_ref/
│   ├── 16_misc/
│   ├── 17_net_infer/
│   ├── 18_http_get/
│   └── 19_multicast/
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

### 03_stdin_echo — 標準入力エコー
`sys_read(fd=0)` で標準入力から 1 行読み込み、そのままエコーするサンプルです。
空行を入力すると終了します。
→ stdin syscall の動作を最もシンプルに確認できます。

### 04_readdir — ディレクトリ一覧
`sys_readdir(path, buf, max)` でディレクトリのエントリ一覧を取得するサンプルです。
ファイル名・サイズ・種別（ファイル/ディレクトリ）を表示します。
→ ファイルシステム操作の仕上げとして試してください。

### 05_rtos_task — RTOS タスク管理
T-Kernel ネイティブ API でタスクを作成・起動します。
優先度スケジューリング（FIFO）とタイムスライス（RR）の両方を実演します。

### 06_rtos_sync — 同期プリミティブ
セマフォとイベントフラグを使ったタスク間同期を実演します。
複数タスクが協調して動作するパターンを学べます。

### 07_mutex — Mutex (相互排他)
`tk_cre_mtx` / `tk_loc_mtx` / `tk_unl_mtx` / `tk_del_mtx` を使って
2つのタスクが共有カウンタを mutex で保護するサンプルです。
優先度継承 (TA_INHERIT) や上限プロトコル (TA_CEILING) にも対応しています。

### 08_task_ext — タスク補助 API
`tk_get_tid` / `tk_sus_tsk` / `tk_rsm_tsk` / `tk_rel_wai` /
`tk_can_wup` / `tk_ter_tsk` を実演します。
タスクの強制サスペンド・レジューム・終了と待ち解除を確認します。

### 09_mailbox — Mailbox (メッセージパッシング)
`tk_cre_mbx` / `tk_snd_mbx` / `tk_rcv_mbx` / `tk_del_mbx` を使い、
タスク間でポインタベースのメッセージを受け渡しするサンプルです。
ユーザー定義メッセージ構造体の先頭に `PK_MSG` を埋め込む方法を示します。

### 10_msgbuf — Message Buffer (可変長メッセージ)
`tk_cre_mbf` / `tk_snd_mbf` / `tk_rcv_mbf` / `tk_del_mbf` を使い、
バイト列メッセージをバッファ経由でコピー転送するサンプルです。
mailbox とは異なりメッセージの「中身」がコピーされます。

### 11_rendezvous — ランデブーポート
`tk_cre_por` / `tk_cal_por` / `tk_acp_por` / `tk_rpl_rdv` / `tk_del_por` を実演します。
カーラータスクがメッセージを送り、アクセプタタスクが受け取って返答するシナリオです。

### 12_mempool — Memory Pool (メモリ管理)
可変長プール (`tk_cre_mpl` / `tk_get_mpl` / `tk_rel_mpl` / `tk_del_mpl`) と
固定長プール (`tk_cre_mpf` / `tk_get_mpf` / `tk_rel_mpf` / `tk_del_mpf`) の
両方をユーザーバッファで使うサンプルです。

### 13_time — 時刻 API
`tk_get_tim`（システム時刻取得）と `tk_dly_tsk`（タスク遅延）を実演します。
`dly_tsk(200)` の前後で経過時間を計測して精度を確認します。

### 14_cyc_alm — Cyclic/Alarm Handler (時間駆動)
周期ハンドラ (`tk_cre_cyc` / `tk_sta_cyc` / `tk_stp_cyc` / `tk_del_cyc`) と
アラームハンドラ (`tk_cre_alm` / `tk_sta_alm` / `tk_del_alm`) の両方を実演します。
ハンドラはタスク独立文脈で実行されるため、ブロッキング呼び出しは禁止されています。

### 15_ref — Ref API + システム情報
sem / flg / mtx / mbx / mbf / mpl / mpf / cyc / alm の全 ref 関数と
`tk_ref_ver`（バージョン情報）・`tk_ref_sys`（システム状態）を実演します。

### 16_misc — 残余 API
`tk_ref_por`（ポート状態参照）・`tk_get_otm`（単調増加稼働時間）・`tk_set_tim`（時刻設定）・
`tk_dis_dsp` / `tk_ena_dsp`（ディスパッチ禁止/許可）・`tk_rot_rdq`（レディキュー回転）・
`tk_exd_tsk`（タスク終了+削除）を実演します。
T-Kernel 2.0 ユーザー空間 API のほぼ全てがこのサンプルで網羅されます。

### 17_net_infer — AI 推論 + UDP ネットワーク配信
センサーデータを AI で分類し、結果を UDP で配信するサンプルです。
カーネル内蔵の MLP ニューラルネットワーク (`SYS_INFER`) と
非同期 AI ジョブ (`SYS_AI_SUBMIT` / `SYS_AI_WAIT`)、
UDP 送受信 (`SYS_UDP_BIND` / `SYS_UDP_SEND` / `SYS_UDP_RECV`) を使います。

### 18_http_get — HTTP GET (TCP クライアント)
`sys_tcp_connect` / `sys_tcp_write` / `sys_tcp_read` / `sys_tcp_close` を使って
HTTP/1.0 GET リクエストを 10.0.2.2:80 に送るサンプルです。
→ TCP syscall の一連の流れを学べます。`make run-disk-net` でネットワーク付き起動が必要です。

### 19_multicast — UDP マルチキャスト
`sys_udp_join_group` / `sys_udp_leave_group` でマルチキャストグループ (239.1.1.1:4000) に
参加・脱退するサンプルです。送信タスクが 3 回マルチキャスト送信し、
ソフトウェアループバックで同一ノード内でも受信できることを確認します。

---

## ビルド方法 (x86)

```bash
# x86 向けに全 ELF をビルド
cd userland/x86
make

# 個別ビルド
make 01_hello/hello.elf
make 02_posix_io/posix_io.elf
make 03_stdin_echo/stdin_echo.elf
make 04_readdir/readdir.elf
make 05_rtos_task/rtos_task.elf
make 06_rtos_sync/rtos_sync.elf
make 07_mutex/mutex.elf
make 08_task_ext/task_ext.elf
make 09_mailbox/mailbox.elf
make 10_msgbuf/msgbuf.elf
make 11_rendezvous/rendezvous.elf
make 12_mempool/mempool.elf
make 13_time/time.elf
make 14_cyc_alm/cyc_alm.elf
make 15_ref/ref.elf
make 16_misc/misc.elf
make 17_net_infer/net_infer.elf
make 18_http_get/http_get.elf
make 19_multicast/multicast.elf
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
p-kernel> exec net_infer.elf
p-kernel> exec stdin_echo.elf
p-kernel> exec http_get.elf
p-kernel> exec readdir.elf
p-kernel> exec mutex.elf
p-kernel> exec mailbox.elf
p-kernel> exec msgbuf.elf
p-kernel> exec mempool.elf
p-kernel> exec cyc_alm.elf
p-kernel> exec task_ext.elf
p-kernel> exec time.elf
p-kernel> exec rendezvous.elf
p-kernel> exec ref.elf
p-kernel> exec misc.elf
p-kernel> exec multicast.elf
```

`/etc/init.rc` を disk.img に置くと、シェル起動時に自動実行されます:

```
# /etc/init.rc
exec multicast.elf
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
  タスク補助:   tk_ter_tsk, tk_sus_tsk, tk_rsm_tsk,
                tk_frsm_tsk, tk_rel_wai, tk_get_tid, tk_can_wup
  セマフォ:     tk_cre_sem, tk_del_sem, tk_sig_sem, tk_wai_sem, tk_ref_sem
  イベントフラグ: tk_cre_flg, tk_del_flg, tk_set_flg,
                  tk_clr_flg, tk_wai_flg, tk_ref_flg
  Mutex:        tk_cre_mtx, tk_del_mtx, tk_loc_mtx, tk_unl_mtx, tk_ref_mtx
  Mailbox:      tk_cre_mbx, tk_del_mbx, tk_snd_mbx, tk_rcv_mbx, tk_ref_mbx
  MsgBuffer:    tk_cre_mbf, tk_del_mbf, tk_snd_mbf, tk_rcv_mbf, tk_ref_mbf
  MemPool(変):  tk_cre_mpl, tk_del_mpl, tk_get_mpl, tk_rel_mpl, tk_ref_mpl
  MemPool(固):  tk_cre_mpf, tk_del_mpf, tk_get_mpf, tk_rel_mpf, tk_ref_mpf
  周期ハンドラ: tk_cre_cyc, tk_del_cyc, tk_sta_cyc, tk_stp_cyc, tk_ref_cyc
  アラーム:     tk_cre_alm, tk_del_alm, tk_sta_alm, tk_stp_alm, tk_ref_alm
  時刻:         tk_get_tim, tk_dly_tsk
  ランデブー:   tk_cre_por, tk_del_por, tk_cal_por, tk_acp_por,
                tk_fwd_por, tk_rpl_rdv
  システム情報: tk_ref_ver, tk_ref_sys
  残余/制御:    tk_ref_por, tk_get_otm, tk_set_tim,
                tk_dis_dsp, tk_ena_dsp, tk_rot_rdq, tk_exd_tsk

ネットワーク API (0x200+):
  sys_udp_bind, sys_udp_send, sys_udp_recv
  sys_udp_join_group, sys_udp_leave_group
  sys_tcp_connect, sys_tcp_write, sys_tcp_read, sys_tcp_close
  sys_mount, sys_umount

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
| 0x130  | SYS_TK_CRE_MTX   | Mutex 作成                  |
| 0x131  | SYS_TK_DEL_MTX   | Mutex 削除                  |
| 0x132  | SYS_TK_LOC_MTX   | Mutex ロック                |
| 0x133  | SYS_TK_UNL_MTX   | Mutex アンロック            |
| 0x140  | SYS_TK_CRE_MBX   | Mailbox 作成                |
| 0x141  | SYS_TK_DEL_MBX   | Mailbox 削除                |
| 0x142  | SYS_TK_SND_MBX   | メッセージ送信              |
| 0x143  | SYS_TK_RCV_MBX   | メッセージ受信              |
| 0x150  | SYS_TK_CRE_MBF   | Message Buffer 作成         |
| 0x151  | SYS_TK_DEL_MBF   | Message Buffer 削除         |
| 0x152  | SYS_TK_SND_MBF   | メッセージ送信（コピー）    |
| 0x153  | SYS_TK_RCV_MBF   | メッセージ受信（コピー）    |
| 0x160  | SYS_TK_CRE_MPL   | 可変長メモリプール作成      |
| 0x161  | SYS_TK_DEL_MPL   | 可変長メモリプール削除      |
| 0x162  | SYS_TK_GET_MPL   | ブロック取得                |
| 0x163  | SYS_TK_REL_MPL   | ブロック返却                |
| 0x168  | SYS_TK_CRE_MPF   | 固定長メモリプール作成      |
| 0x169  | SYS_TK_DEL_MPF   | 固定長メモリプール削除      |
| 0x16A  | SYS_TK_GET_MPF   | ブロック取得                |
| 0x16B  | SYS_TK_REL_MPF   | ブロック返却                |
| 0x170  | SYS_TK_CRE_CYC   | 周期ハンドラ作成            |
| 0x171  | SYS_TK_DEL_CYC   | 周期ハンドラ削除            |
| 0x172  | SYS_TK_STA_CYC   | 周期ハンドラ開始            |
| 0x173  | SYS_TK_STP_CYC   | 周期ハンドラ停止            |
| 0x178  | SYS_TK_CRE_ALM   | アラームハンドラ作成        |
| 0x179  | SYS_TK_DEL_ALM   | アラームハンドラ削除        |
| 0x17A  | SYS_TK_STA_ALM   | アラームハンドラ開始        |
| 0x17B  | SYS_TK_STP_ALM   | アラームハンドラ停止        |
| 0x109  | SYS_TK_TER_TSK   | タスク強制終了              |
| 0x10A  | SYS_TK_SUS_TSK   | タスク強制待ち              |
| 0x10B  | SYS_TK_RSM_TSK   | タスク強制待ち解除          |
| 0x10C  | SYS_TK_FRSM_TSK  | タスク強制待ち解除（強制）  |
| 0x10D  | SYS_TK_REL_WAI   | 待ち状態解除                |
| 0x10E  | SYS_TK_GET_TID   | 自タスク ID 取得            |
| 0x10F  | SYS_TK_CAN_WUP   | 起床要求キャンセル          |
| 0x114  | SYS_TK_REF_SEM   | セマフォ状態参照            |
| 0x125  | SYS_TK_REF_FLG   | イベントフラグ状態参照      |
| 0x134  | SYS_TK_REF_MTX   | Mutex 状態参照              |
| 0x144  | SYS_TK_REF_MBX   | Mailbox 状態参照            |
| 0x154  | SYS_TK_REF_MBF   | Message Buffer 状態参照     |
| 0x164  | SYS_TK_REF_MPL   | 可変長プール状態参照        |
| 0x16C  | SYS_TK_REF_MPF   | 固定長プール状態参照        |
| 0x174  | SYS_TK_REF_CYC   | 周期ハンドラ状態参照        |
| 0x17C  | SYS_TK_REF_ALM   | アラームハンドラ状態参照    |
| 0x180  | SYS_TK_GET_TIM   | システム時刻取得            |
| 0x181  | SYS_TK_DLY_TSK   | タスク遅延                  |
| 0x190  | SYS_TK_CRE_POR   | ランデブーポート作成        |
| 0x191  | SYS_TK_DEL_POR   | ランデブーポート削除        |
| 0x192  | SYS_TK_CAL_POR   | ランデブー呼び出し          |
| 0x193  | SYS_TK_ACP_POR   | ランデブー受け付け          |
| 0x194  | SYS_TK_FWD_POR   | ランデブー転送              |
| 0x195  | SYS_TK_RPL_RDV   | ランデブー返答              |
| 0x1A0  | SYS_TK_REF_VER   | T-Kernel バージョン参照     |
| 0x1A1  | SYS_TK_REF_SYS   | システム状態参照            |
| 0x196  | SYS_TK_REF_POR   | ランデブーポート状態参照    |
| 0x182  | SYS_TK_GET_OTM   | 単調増加稼働時間取得        |
| 0x183  | SYS_TK_SET_TIM   | システム時刻設定            |
| 0x1C0  | SYS_TK_EXD_TSK   | タスク終了+削除             |
| 0x1C1  | SYS_TK_DIS_DSP   | ディスパッチ禁止            |
| 0x1C2  | SYS_TK_ENA_DSP   | ディスパッチ許可            |
| 0x1C3  | SYS_TK_ROT_RDQ   | レディキュー回転            |
| 0x207  | SYS_UDP_JOIN_GROUP | マルチキャストグループ参加  |
| 0x208  | SYS_UDP_LEAVE_GROUP| マルチキャストグループ脱退  |
| 0x300  | SYS_MOUNT        | マウント状態確認            |
| 0x301  | SYS_UMOUNT       | アンマウント（ダミー）      |
| 0x210  | SYS_INFER        | MLP 推論（同期）            |
| 0x211  | SYS_AI_SUBMIT    | AI ジョブ投入（非同期）     |
| 0x212  | SYS_AI_WAIT      | AI ジョブ完了待ち           |
