# S1: 外した端末に群れのものが残らない（設計）
<!-- vmodel rung: PARTIAL -->
<!-- vmodel certs: [swarm-leave] -->

2026-11-15 10:00 の回（ROADMAP 0-14 S1、#43 の 3）。試験を先に書き（bbe28579、master で全部赤）、同じ回で実装した（§5、監査待ち）。hosted だけ（ベアは保存先が無い）。

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

## 5. 実装（2026-11-15 10:00 の回、feat/s1-leave、監査待ち）

- `arch/common/swarm_leave.c`（hosted のビルドだけに入る。Makefile 3 つと Android の CMake）、pfs_durable.c に `.org` の読み書き・脇へ移す・名前と origin を回す、pfs_block.c の hosted の部分で `.org` を書き（自分は 0xFF）・読み戻しで使い・drop で消す。usermain（x86_64・aarch64）に `leave`・`join`・`pfs origins` と cmd_net の門
- `leave` の後、この起動のうちは pfs_dur_write が mind.rw・ns1_student.bin を書かない（RAM の重みの書き戻しを止める）。**この守りを見る試験は無い**: L2 の赤 3 回はどれも `join` の後の起動で生まれた赤子が自分の重みを保存したもので、試験の見る時点の誤りだった（daaf3a62 で直した。d7611766 の「書き込みが leave と競う」という読みは誤り）
- 測り（容器、x86_64）: 素 L1〜L5 緑、陰性 3 つはそれぞれ狙いの項目が赤、crown（ベア x86・aarch64 の .text・.rodata）は master と全バイト同じ、hosted の警告 57＝master と同じ

## 6. 監査-181（2026-11-15 12:30 の回、実装していない回）: **FAIL（実装側へ戻す）**

再現（容器）: 先端 7c473d32 ALL PASS、試験だけの bbe28579 は全部赤、陰性 3 つとも狙いが赤。[n2-mix] XP+XQ ALL PASS、protect test・pfs flattest・run_knowledge_survival.sh も緑（origin を読み戻すようにした影響は見えない）。

1. **戻す理由: `leave` の後に届いた他人の塊が、外れた後の起動に残る。** `leave`→`leave seed 300`（網の受け pfs_repl.c 710 行と同じ pfs_put_origin）→再起動で `[pfs] origins self 0 foreign 1`、ファイルも残る。網の受けは止まり（stop）を見ない＝`leave` から再起動までの間に仲間の押しや want の答えが来ると、群れの物が残る。おすすめの直し: 印（swarm-left.st）のある起動で、他人の塊をもう一度消す（`leave` の途中で落ちた時も救える）。あわせて印のある間は pfs_put_origin が他人の塊を保存しない。試験 L6: 上の手順で foreign 0
2. **試験の穴（直しと一緒に）**: 自分の塊を 0xFF でなく番号で書く壊し（pfs_block.c の三項を外す）で L1〜L5 が全部緑。だがその壊しでは、網のある起動で `pfs put` した自分の記録が、網のない起動の `leave` で**消える**（drpc_my_node が 0xFF のため番号 1 が他人に見える）。素では残る（.org=ff00、番号 1→2 でも残る）。L1 の自分の塊を `pfs put`（網あり）で作る形にすれば赤になる
3. 限界（直さなくてよいが書く）: pfs_dur_foreach_origin は 4096 個で黙って打ち切る。固定の失敗の後の書き直し（pfs_block.c 240 行）は .org を書かない＝分からない塊として脇へ（安全側）
