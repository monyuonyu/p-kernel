# survival-fs — ARK: the local filesystem that survives the flood

> p-kernel は「全体で1つの脳」であり、その記憶は滅びてはならない。
> しかし最下層の永続化はいまだ **FAT32** — 上書きで過去を壊し、電源喪失で
> FAT を破損させ、版を残さず、自己検証も持たない、思想と無関係なフォーマットである。
> このドキュメントは、その下層を **ARK**（内容アドレス・追記専用ログ・アトミック
> コミット・自己検証）へ置き換えるための設計と試作の記録である。
> ARK は「群れ全体の記憶」である `p-fs`（[[p-fs.md]]）の **1ノード・ローカルな
> 永続バックエンド**であり、同じ content-addressed ブロックを土台に両者は統合される。

Status: **試作あり（hosted で動作）+ ベアメタルはビルド通過の opt-in** / 最終更新: 2026-06-07
担当: 第12波・FS研究開発隊 / ブランチ: `w12-survival-fs`

関連: [[p-fs.md]]（分散・gossip 複製層。ARK はその durable backend）、
[[survival-network.md]]（§9「保存の図書館 → 考える器官」、G24「図書館が揮発メモリ」）、
[[regions.md]]（局所性）、`arch/common/pfs_block.c`（content-addressed block の先行実装）、
`arch/x86/fat32.c`（置き換え対象）。

---

## 0. なぜ FAT32 は思想にふさわしくないか（file:line で）

`arch/x86/fat32.c`（1030行）は disk.img 上の FAT32 ドライバで、POSIX 風 VFS
（`arch/common/include/vfs.h`）の裏を `arch/x86/vfs.c` 経由で実装する。
`vfs.h` 自身がその貧しさを白状している：

> `vfs.h:4-7` — *"Currently: one mount point (FAT32 on IDE drive). Future:
> multiple backends (ramfs, devfs, …)."*

FAT32 が「ノード死耐性のある脳の記憶」にとって致命的な理由を、コードの実体で示す。

### 0.1 上書き破壊（既存データを物理的に潰す）
FAT32 はブロックを **その場で上書き**する。`fat32.c` の全ての書き込みは
`blk_write(lba, …)`（`fat32.c:28` のマクロ）で **既存セクタを直接破壊**する：

- **FAT そのものの上書き**: `fat_set()`（`fat32.c:545`）が
  `blk_write(fat_sector, …)`（`fat32.c:552`、ミラーは `:554`）でクラスタ連鎖を
  その場で書き換える。書き込み途中の電源喪失で **FAT が壊れる**（torn write が
  ファイルシステムの根幹を直撃する）。
- **ディレクトリエントリの上書き**: 作成/サイズ更新/削除/rename がすべて
  `blk_write(entry_lba, 1, dir_sec_buf)` で同一セクタを上書きする
  （`fat32.c:788, :800, :814, :914, :978, :1024, :1028`）。
- **データクラスタの上書き**: `write_cluster()` 経由 `blk_write(cluster_to_lba…)`
  （`fat32.c:593`）。`fds[].size = de->file_size`（`:524`）のように
  **「最新の1枚」しか持たない**。

→ `survival-network.md §9`「記憶がなければ考えられない」その記憶が、
**履歴を残さず書き潰される**。

### 0.2 電源喪失耐性が無い（fsck 前提）
FAT32 には**チェックポイントもアトミックコミットも無い**。書き込みは
「FAT を更新し、ディレクトリを更新し、データを書く」という**複数の独立した
上書き**から成り、その途中で電源が落ちれば不整合になる。`fat32.c` には
チェックサム/ハッシュが**1つも無い**（grep して `crc`/`sha`/`hash`/`checksum`
は皆無）。よって**破損を検出する手段も、安全な再生（replay）も無い**。
これは `survival-network.md` G24「図書館が揮発メモリである」という不変条件違反の
最下層の現れである。

### 0.3 版を残さない・自己検証が無い・中央的な木
- **版を残さない**: 上書きモデルゆえ、過去版は存在しない（`fat32.c:524` が
  `file_size` を「現在値」で握るのみ）。「古いものも生き残る図書館」になれない。
- **自己検証が無い**: ブロックに id もチェックサムも無いので、ビット腐敗
  （bit rot）を**検出できない**。読めば黙って壊れた値を返す。
- **中央的なディレクトリ木**: `root_cluster`（`fat32.c:240`）を根とする単一の
  木。位置非依存の名前が無く、複数ノードが同じ内容を共有・マージする素地が無い
  （`p-fs.md §0.2` の指摘と同根）。

`pfs_block.c:149` の TODO —「*also append (id,len,bytes) to a content-addressed
file on FAT32 here*」— は、まさにこの貧しい下層を content-addressed な
durable store に置き換えたいという未完の意志である。**ARK がその答え**である。

---

## 1. 名前と設計判断 — なぜ「ARK」か

名前は **ARK**（モジュール `arch/common/arkfs.c` / `arkfs.h`、シンボル `ark_*`、
オンディスク magic `"ARKLOG01"`）。

由来は二重である：
1. **方舟（Noah's Ark）** — 大洪水（＝電源喪失・torn write・ノード死）を**乗り越えて
   記憶を運ぶ器**。`survival-network.md` の「滅びないこと」を最下層で体現する比喩。
2. **頭字語 A.R.K.** — **A**ppend-only（追記専用）/ **R**eplayable（ログ再生で
   復旧）/ **K**eep-everything（版を捨てない図書館）。ARK の3性質そのもの。

`sfs`（Shared Folder Sync, `arch/common/sfs.c`）・`pfs`（distributed, `pfs_block.c`）と
衝突しない短い名前を選んだ。ARK は **ローカル・永続層**、pfs は **分散・群れ層**で、
両者は同じ content address を共有する（§7）。

### 採否した設計原則と理由
| 原則 | 採否 | 理由 |
|---|---|---|
| **Content-addressed**（block-id = sha256） | **採用** | 重複排除と自己検証が無料。`pfs_block` と**同じ32B id 空間**で統合できる（§7）。`relay/sha256.c` のゼロ依存実装を流用。 |
| **Log-structured / append-only** | **採用** | 既存バイトを決して上書きしない → torn write は**末尾しか壊せない**。FAT32 §0.1 の真逆。 |
| **Copy-on-write** | **実質採用** | 更新は新 block + 新 commit を**追記**し、旧版はログに残る（CoW の効果を log-structured で得る）。 |
| **アトミック・チェックポイント（commit record）** | **採用** | 「新版が live になる」唯一の点。crc + sha256 で torn commit を弾く（§5）。 |
| **自己検証**（全 block に sha256 + crc32） | **採用** | fsck 不要。読み出し時に bit rot を検出（§5.3）。 |
| **版を刻む（immutable/versioned）** | **採用** | 旧 commit はログに残り続ける＝図書館。`ark_read_version` で過去を読める。 |
| **B-tree/extent 等の高度なインデックス** | **不採用（設計に留める）** | 試作はフラットなパス文字列表＋線形ブロック索引で十分。規模が要求してから（§8）。 |
| **消失訂正符号（erasure coding）** | **不採用（p-fs 側 P4 の責務）** | ローカル単体の生存には全 block 保持で足りる。分散冗長は `p-fs.md §2.4`。 |

---

## 2. オンディスク・レイアウト

全て **セクタ整列**（`ARK_SECTOR = 512`）。固定幅型（U1/U2/U4）のみを使い、
`_Static_assert` で全構造体サイズを固定（LP64 罠回避。`feedback_lp64_typedef_trap`）。
ABI 非依存なので **同じイメージが aarch64 / x86_64 / i686 で同一にマウント**する。

```
 sector 0      : superblock     ("ARKLOG01", geometry, epoch, crc32)  不変
 sector 1..    : the LOG — 追記専用のレコード列
                  REC_BLOCK   1個の content-addressed データ block
                  REC_COMMIT  ファイルシステム全体のチェックポイント
```

各レコード = **1 ヘッダセクタ + ceil(payload/512) ペイロードセクタ**。

### 2.1 superblock（sector 0、format 時に1回だけ書く）
```c
struct ark_super {           // 32 bytes（_Static_assert で固定）
  U1 magic[8];   // "ARKLOG01"
  U4 version;    // フォーマット版
  U4 sector_size;// 512
  U4 log_start;  // 最初のログセクタ（=1）
  U4 total_sectors;
  U4 epoch;      // ★format 世代。全レコードに刻まれ、再生時に照合される
  U4 crc;        // 上記バイト列の crc32
};
```
`epoch` は **再フォーマットや過去の残骸からの誤再生を防ぐ要石**である（§5.4）。

### 2.2 レコードヘッダ
```c
struct ark_rec_hdr {         // 60 bytes（固定）
  U1 magic[4];   // "ARKR"
  U4 type;       // REC_BLOCK | REC_COMMIT
  U4 seq;        // epoch 内の単調増加シーケンス
  U4 epoch;      // superblock.epoch と一致しなければ無効
  U4 len;        // payload バイト長
  U1 id[32];     // sha256(payload) — block-id（自己検証の核）
  U4 payload_crc;// crc32(payload)
  U4 hdr_crc;    // crc32(これ以前のヘッダバイト)
};
```
**ヘッダ妥当性**（magic+hdr_crc+epoch）と **ペイロード妥当性**（payload_crc かつ
sha256(payload)==id）を**別々に判定**するのが ARK 復旧の肝である（§5.3）。

### 2.3 REC_COMMIT のペイロード = ディレクトリ表のスナップショット
```c
struct ark_commit_hdr { U4 commit_seq; U4 nent; };
struct ark_dent {            // 592 bytes（固定）
  char name[64];             // フルパス "/logs/a.txt"
  U1   is_dir; U1 pad[3];
  U4   version;              // このファイルの版番号（1,2,3…）
  U4   size;
  U4   nblk;                 // 内容 block 数
  U1   blk[16][32];          // 各 block の sha256 id（最大 64KiB/file）
};
```
commit は **その瞬間の全 live エントリ**を内包する。これにより「現在の世界」を
1つのアトミックなレコードで確定できる。

### 2.4 ディレクトリ表現（試作）と設計上の本来形
- **試作**: フラットなパス文字列の表。`ark_readdir(path)` は接頭辞照合で直下の
  子だけを返す。中央の inode 割当表を持たない（principle 5: マージ親和）。
- **本来形（設計に留め）**: 各ディレクトリ自身を content-addressed object
  （子の {name→object-id} マニフェスト）にする **Merkle ディレクトリ木**。
  これにより部分木を内容アドレスで共有・マージでき、`p-fs` の object 層
  （`p-fs.md §3.1`）と完全に同型になる。試作はここを**意図的に単純化**した。

---

## 3. 書き込み経路（save、各操作 = 1 アトミックコミット）

`ark_write_file(path, buf, len)`:
```
1. buf を <=4096B の chunk に分割。各 chunk を block 化（block-id = sha256）。
   既に索引にある id は再書き込みしない（重複排除）。新規 block を LOG に追記。
2. live ディレクトリ表のコピーを更新（path があれば version+1、無ければ version=1、
   size/nblk/block-id 列を差し替え）。★この時点では disk 上の live はまだ旧版。
3. 更新後スナップショットを REC_COMMIT として LOG に追記し、fsync。
   ── これが唯一のアトミック可視化点。commit が valid に着地して初めて新版が live。
```
`mkdir`/`unlink` も同じく「表を変えて commit」する1コミット操作。
**「特別なフォルダ」も「保存と公開の別操作」も無い**（`p-fs.md §0.3` の断絶を
ローカル層から消す。クラスタ公開は §7 の hook で p-fs に委ねる）。

---

## 4. 読み出し経路（自己検証つき）

`ark_read_file(path)`:
```
live 表から path を引き、その block-id 列について
  各 block を索引から読み、payload を sha256 し id と照合
  不一致なら ARK_E_CORRUPT を返す（fsck 不要の腐敗検出）
```
`ark_read_version(path, v)` / `ark_history(path)` は **ログ中の全 commit を走査**して
過去版を再構成する。旧 commit と旧 block は**不変でログに残り続ける**ので、
**上書き後も過去版をバイト単位で読める**＝図書館。

---

## 5. クラッシュ整合性の論証（なぜ電源喪失で壊れないか）

### 5.1 不変条件
1. **追記専用**: 既存バイトを上書きしない。書き込みはログ末尾への追記のみ。
2. **アトミックコミット**: 新版は、その REC_COMMIT が
   `hdr_crc` ∧ `payload_crc` ∧ `sha256(payload)==id` を**全て満たして着地した瞬間**に
   だけ live になる。

### 5.2 帰結（torn write は末尾しか壊せない）
電源喪失で壊れ得るのは**書き込み途中のレコード（＝ログ末尾）だけ**である。
過去の commit と block は追記専用ゆえ**物理的に無傷**。3つの場合に尽きる：

| クラッシュ点 | ディスク上の結果 | 再マウント時の判定 | 帰結 |
|---|---|---|---|
| **data block 追記中** | 末尾に torn block、commit 無し | 後続に valid commit が無い → pending を破棄 | **前の commit に巻き戻る** |
| **commit 書き込み途中（torn commit）** | commit の payload_crc 不一致 | `payload_ok=0` → その commit を**採用しない** | **前の commit に巻き戻る** |
| **commit 完全着地後** | valid な新 commit | 採用 | **新版が durable** |

→ 観測される状態は常に **「直前の版が完全」か「新版が完全」**のどちらか。
**半端な状態は原理的に存在しない**。

### 5.3 ヘッダ妥当性とペイロード妥当性の分離（rot を read で捕まえる）
復旧スキャンは **ヘッダが valid なら len だけ進む**。よって 1個の腐敗 block が
ログ全体を切り詰めることはない。腐敗 block は（commit 済みなら）索引には載るが、
**読み出し時に sha256 照合で `ARK_E_CORRUPT`** になる。一方 **torn header**
（magic/hdr_crc/epoch 不一致）はスキャンを停止させる＝真のログ末尾。
この分離が「rot は read で検出」「torn tail は巻き戻し」を両立させる。

### 5.4 epoch — 過去の残骸からの誤再生を防ぐ
追記専用ログは、再フォーマットや別世代の残骸が**末尾の先に valid な姿で
残り得る**。`format` は superblock の epoch を**前世代+1**に上げ、全レコードに刻む。
スキャンは `epoch != superblock.epoch` のレコードを**末尾として扱い停止**する。
これが無いと、同一ディスク上の旧データを新版と誤認し得る（実際テストで踏んだ罠）。

### 5.5 実証（samples/25_survival_fs/run.sh、生ログ）
ブロックデバイス層に **本物の SIGKILL** を注入（電源喪失は物理的にデバイスで
起きる）。`v1` を durable にしてから、`v2` 書き込みの**全デバイス書き込み点**で
プロセスを別個に kill し、別プロセスで再マウント・読み出し：

```
  kill=TORN   #1  writer_rc=137  remount: rolled back to v1 (intact)
  kill=TORN   #2  writer_rc=137  remount: rolled back to v1 (intact)
  kill=TORN   #3  writer_rc=137  remount: rolled back to v1 (intact)
  kill=TORN   #4  writer_rc=137  remount: rolled back to v1 (intact)
  kill=TORN   #5  writer_rc=137  remount: committed v2 fully (intact)
  ...
  kill=BEFORE #4  writer_rc=137  remount: rolled back to v1 (intact)
  kill=BEFORE #5  writer_rc=137  remount: committed v2 fully (intact)
  PASS  every crash point left a clean, fully-consistent store

  -- torn-commit の生トレース --
  before crash: READ: GOOD-V1 / VERSION: 1
  [inject] torn half-sector then SIGKILL at write #4 (lba 11)
  writer exited rc=137 (137 = SIGKILL)
  remount: READ: GOOD-V1 / VERSION: 1
  PASS  torn commit rolled back to GOOD-V1
```
全ての kill 点で状態は v1 完全 or v2 完全、**破損ゼロ**。2回連続 PASS。

---

## 6. VFS 互換の取り方

ARK の whole-file API（`ark_write_file/read_file/stat/mkdir/unlink/readdir`）は
`vfs.h` の意味論にそのまま対応する。**既存の x86 boot は壊さない**ため、
`arch/x86/vfs.c` の既定（FAT32）は**一切変更しない**。統合の道筋は：

- `vfs.h` は既に第2バックエンドの枠 `VFS_BACKEND { FAT32, PFS }` を予約済み
  （`vfs.h:85-88`）。ここに `VFS_BACKEND_ARK` を足し、`vfs.c` をマウントポイント
  ごとのディスパッチ表にするだけで、**API 変更なし**で ARK に載る。
- fd 流（open/seek/write/close）は、ARK の whole-file 上に薄いアダプタ
  （open=スナップショット参照、create→close でバッファして1コミット）で被せられる。
  試作では whole-file API で CRUD/版/readdir を満たし、fd アダプタは設計に留めた。
- ベアメタルでは `ARK_BDEV`（read/write/sync の ops 構造体）に
  `arch/x86/include/blk_ssy.h` の `ide0` を1関数で橋渡しすれば動く（arch/common を
  汚さないため ARK_BDEV は blk_ssy とは別定義にしてある）。

---

## 7. p-fs との統合 — ARK が p-fs の durable backend になる道筋

ARK の block-id は `pfs_block` と**同一**（どちらも `sha256(bytes)` の32Bバイト列、
`pfs_block.h` と `arkfs.h` 双方が `_Static_assert(... == 32)`）。よって
**翻訳ゼロ**で噛み合う。ARK は pfs-互換の raw block API を公開している：

```c
INT ark_block_put(const void *buf, U4 len, U1 id_out[32]);  // = pfs_put 相当
INT ark_block_get(const U1 id[32], void *buf, U4 max);      // = pfs_get 相当
INT ark_block_has(const U1 id[32]);
```

統合は2方向の一行接続で成立する（E隊の pfs durable 化と**衝突しない別モジュール**
として実装済み。実配線は指揮官/E隊が行う想定で、ここでは設計＋互換APIまで）：

1. **save==publish（put hook）**: `pfs_set_put_hook(...)`（`pfs_block.h:64`）に
   「ARK へ durable 追記する shim」を登録すれば、**メモリ上の pfs block が
   そのまま ARK のログに永続化**される。`pfs_block.c:149` の TODO の解。
2. **durable backend（get fallback）**: `pfs_get` がメモリ表でミスしたとき
   `ark_block_get` を引けば、**再起動を跨いで block が残る**（pfs P0 は今 in-memory
   table のみ＝再起動で消える。ARK がその永続層になる）。

こうして「ローカルにファイルを書く（ARK）」と「群れに公開する（pfs gossip）」が
**同じ content-addressed block の海**の上で一つになる（`p-fs.md §1` の save==publish
を、最下層の durable 層から支える）。erasure coding / 分散ルックアップは p-fs 側の
責務（`p-fs.md §2.4, §3.3`）であり ARK は触れない。

---

## 8. 何を試作し、何を設計に留めたか（正直に）

### 試作した（hosted で実動、4ターゲットでビルド通過）
- `arch/common/arkfs.c` / `arkfs.h`：log-structured + content-addressed +
  アトミックコミット + epoch + 自己検証 + 版管理 + 重複排除 の**フル機能コア**。
- `ARK_BDEV`（file/RAM 裏のブロックデバイス抽象）。
- CRUD / readdir / `ark_version` / `ark_history` / `ark_read_version`。
- pfs-互換 raw block API（`ark_block_put/get/has`）。
- `ark_self_test`（RAM 裏、ロジックレベル）+ `samples/25_survival_fs`（file 裏、
  **本物の SIGKILL によるクラッシュ整合性**・dedup・rot 検出）。

### 設計に留めた（理由つき）
- **VFS への実ディスパッチ配線**：x86 boot を壊さないため `vfs.c` は不変。枠は
  `vfs.h` に既存。fd 流アダプタも設計のみ（whole-file で要件は満たす）。
- **ベアメタル実マウント**：`ARK_BDEV`↔`blk_ssy` 橋渡しは1関数だが、既存 FAT32
  disk.img を壊さないオプトインに留め、ベアメタルはビルド通過のみ。
- **Merkle ディレクトリ木**：試作はフラットなパス表。本来形は §2.4。
- **ログのガベージコレクション**：append-only は永遠に増える。分散 GC の難しさ
  （`p-fs.md §6.3`）と同型で、当面「捨てない」。容量逼迫検知は `degrade.c` 的
  シグナルに委ねる将来課題。
- **block 単位の上限**：試作は `ARK_MAX_FILES=32`, `ARK_MAX_BLK=16`（64KiB/file），
  索引 256。規模が要求してから可変表へ（鶏と卵への正直な答え＝`regions.md §5` と同型）。
- **p-fs との実 hook 接続**：互換 API までで、`pfs_set_put_hook` への実登録は
  E隊の durable 化と統合する段で（衝突回避）。

### 非目標（embrace）
POSIX 完全互換・強整合・即時可視は目指さない（`p-fs.md §7` と同じ）。
ARK はローカル durable 層として「壊れないこと・過去を残すこと・内容で名指すこと」を
最優先し、分散一貫性は p-fs に委ねる。

---

## 付録 A — 一枚の絵

```
  人間名 "/report.txt"
        │  live 表（最新 commit が握る。可変なのはここだけ）
        ▼
  REC_COMMIT(seq=N) ──parent(暗黙: 旧 commit はログに残る)──▶ COMMIT(N-1) ▶ … ▶ COMMIT(1)
        │ payload = ark_dent[]：name, version, size, blk-id[]   （不変・追記専用）
        ▼
   blkA=sha256(..)   blkB=sha256(..)   …      （不変・内容アドレス・自己検証）
   ───────────────────────────────────────────────────────────
   LOG (append-only):  [sb][C1][blkA][C2][blkB][C3] … ←g_head（次の追記点）
        電源喪失 → 末尾の書きかけだけが壊れる → 再生は最後の valid commit まで
        epoch 照合で過去世代の残骸を弾く / read は sha256 で rot を弾く
   ───────────────────────────────────────────────────────────
   block-id は pfs_block と同一 → ARK は p-fs の durable backend（§7）
```
