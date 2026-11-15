# S1: 外した端末に群れのものが残らない（設計）

2026-11-15 10:00 の回（ROADMAP 0-14 S1、#43 の 3）。試験を先に、実装は次の回から。hosted だけ（ベアは保存先が無い）。

## 1. いま残るもの（master c02cae26 を読んだ）

`$PKERNEL_PFS_DIR` の下に: 塊（名前＝sha256、他人の記録・重みの欠片・入れたコードの単位）、`refs.tab`、`mind.rw`（心の重み＝合体した後は他人の分を含む）、`ns1_student.bin`、`swarm-stop.st`、`node_seed`、`ark.key`。ARK の像（`PKERNEL_ARK_IMG`）にも塊。

**芯の穴**: ディスクには塊の origin が無い。読み戻し（pfs_block.c 569 行）は全部 `PFS_ORIGIN_SELF`＝**再起動すると他人の塊が自分のものになる**（自分の名で告知し直す）。これを直さないと「他人の物だけ消す」ができない。

## 2. 決めたこと

1. **origin を残す**: 塊の横に `<id>.org`（2 B LE の pk_node_t、一時→fsync→rename）。読み戻しで使う。無い塊は「分からない」。**自分の塊は番号でなく 0xFF（self）で書く**＝番号が後で変わっても（node_seed の作り直し・N2c の引き直し）自分の原本が他人にならない
2. **`leave`（コンソールだけ）**: ① 手元の止まり（S2 の `stop now`）② origin が自分でない塊を消す ③ `mind.rw`・`ns1_student.bin` を消す（合体した重みは群れの分を分けられない。次の起動はふだんの事前学習から）④ 印 `swarm-left.st` を fsync＝再起動しても網に出ない。`join` で印を消すまで
3. **自分の原本は消さない**（決まり 11）。origin が自分の塊・`ark.key`・`node_seed` は残す。**分からない塊は消さずに `left-unknown/` へ移し、数を言う**（この変更より前の塊。消すかは持ち主が決める）
4. 出力: `[leave] foreign F weights W unknown U kept K`、`pfs origins` は `[pfs] origins self S foreign F unknown U`
5. 試験のビルド `-DSWARM_LEAVE_TEST` に `leave seed self|<origin>`（その origin で 1 塊を入れ、`[leave] seed id=<64 hex> origin=self|<n>`）。`pfs origins` の self は 0xFF と自分の番号の両方
6. `join` は印と、`leave` が掛けた手元の止まりを解く。網に出るのは次の起動から

## 3. 合否と陰性（作る前に固定）`tests/host/run_swarm_leave.sh`

- **L1** 自分の塊 1・他人（origin 7・300）2 を入れて `leave`→foreign 2。再起動後 `pfs origins` が self 1 foreign 0、自分の塊は同じバイト
- **L2** `leave` の後 mind.rw・ns1_student.bin が無い
- **L3** 印のあるまま `PKERNEL_RELAY` を指して再起動すると、`[leave] left` が出て `[net] transport = ` の行が 0。`join` の後の再起動では出る
- **L4**（前提）`leave` なしで再起動しても他人の塊は foreign 2（master では 0＝赤）
- **L5** `.org` を消した塊は unknown 1 で `left-unknown/` に 1 つ、消えない
- 陰性: `-DS1_NC_LEAVE_KEEP_FOREIGN`（L1 赤）・`-DS1_NC_ORIGIN_VOLATILE`（L4 赤）・`-DS1_NC_LEAVE_NO_FLAG`（L3 赤）

## 4. 限界・まだ

- ARK の像の塊は origin を持たない＝第 1 版は平らな保存先だけ。ARK は次の切れ（像の中に origin の表）
- 他の端末に複製された自分の記録は消せない（頼むのは S2 の道か、別の項目）
- 入れたコードが走っている間に `leave` したときの扱い（再起動を求めるだけ）。genome・selfc の取り込みが塊の外に残るかは実装の回で読む
