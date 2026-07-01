# Dynamic node ID — churn 耐性のある同一性、中央なしへの道筋

> **現在地（2026-07-01・doc-hygiene 追記／本文は 年輪 として保存）:** G6 の第一歩（relay lease による node-id）は方向として生きている。UX キューに **鍵派生 node-id** が積まれている（product-soul／UX）。完全自己編成トポロジは引き続き設計。正本は [[gap-ledger.md]]。

監査 G6（🟡）への第一歩。ノード ID 割り当てを「人間が外から固定」から
「relay が空き ID を貸す（lease）」へ動かし、その先の **完全自己編成
トポロジ**への設計道筋を残す。wire protocol v2（HMAC-SHA256 + nonce
window）は不変。後方互換最優先。

関連: [[archive/philosophy-gap-audit.md]] G6 / [[relay-ha.md]]（relay 冗長化）/
[[decentralized-lookup.md]]（HRW + gossip、churn 前提の責任集合）/
[[regions.md]] / [[survival-network.md]]。

## 1. 問題（G6 の引用）

> **G6**: ノード ID と relay 順序が静的トポロジ。UMP は `PKERNEL_NODE_ID`
> env（1..255）と `PKERNEL_RELAY` の順序付き静的リストで同一性とトポロジを
> 固定する。decentralized-lookup/survival は「ID は churn で動的に割り当て」
> を前提にするが、ID は人間が外から固定。relay-ha は「全ノードが同一順序
> リストを持つ純関数」で収束＝静的設定が疑似中央（リスト先頭=暗黙の優先点）。
> 動的 ID 割り当て・自己編成トポロジは未実装。
> 証拠: `arch/linux/aarch64/net_relay.c:501-514`（env ID）、
> `relay-ha.md:9-15`（順序付き静的リスト）。

二つの静的性が絡んでいる:

1. **ID の静的性** — `PKERNEL_NODE_ID` を人間が決める。新しいノードが
   churn で増えるたびに、誰かが衝突しない番号を手で割り振らねばならない。
   これは [[decentralized-lookup.md]] が前提とする「ID は join/leave で
   動的に動く」と矛盾する。HRW の母集団 `dnode_table[]` は churn で
   変わる想定なのに、その入口（ID 取得）が手動。
2. **トポロジの静的性** — `PKERNEL_RELAY` の順序付きリスト先頭が暗黙の
   優先点。relay 自体は [[relay-ha.md]] で冗長化済みだが「全員が同じ
   先頭を選ぶ」収束規約が、リストという静的設定に依存している。

## 2. 思想が要求するもの

「誰も所有しない、中央のない器」を掲げる以上、同一性の発行もトポロジの
決定も、特定の一点に永続的に依存してはならない。要件:

- **churn 耐性**: ノードはいつでも join/leave/crash する。ID は枯れた
  ら回収され、再利用できる。手動介入ゼロで番号が回る。
- **中央なし（恒久的単一点の不在）**: ID 発行点が一時的に「役割」として
  存在するのは許す（死ねば次が引き継ぐ）。許さないのは「死んだら同一性
  発行そのものが止まる恒久的中央」。これは [[decentralized-lookup.md]]
  §の「region 内の最小 ID という決定的に選ばれる coordinator は中央では
  ない——死ねば次の最小 ID が継ぐ」と同じ論法。
- **後方互換**: `PKERNEL_NODE_ID` を明示する既存運用（固定トポロジの
  デモ・テスト）は一切壊さない。

## 3. ID 割り当ての分散設計 — 二方式の比較

### 3A. relay lease 方式（今波で実装）

relay が `{node_id → addr}` 表を既に持つ。これを ID 配布元に流用する。
クライアントが ID を持たないとき `src=0`（auto）で REGISTER すると、
relay が**未使用の最小 ID** を貸し、grant で返す。解放は idle eviction
（leased slot が `IDLE_TIMEOUT` 無通信 → 回収 → 再利用可）。

- 長所: 状態が既に relay にある。実装が最小（§6）。衝突は relay が単一
  権威なので原理的に起きない。idempotent（同一 addr の再要求は同一 ID）。
- 短所: **relay が ID 発行の一点**になる。relay は第7波で冗長化済みだが、
  冗長 relay は互いに状態を共有しない（[[relay-ha.md]]: 「relay 同士は
  通信せず、ノード表は各 relay が REGISTER から独立に再学習する」）。
  よって**複数 relay 環境での lease 調停**が未解決（§3C）。

### 3B. 完全 P2P 方式（self-assign + 衝突検出）

relay を ID 権威にしない。各ノードが自分で ID を提案し、メッシュ上で
衝突を検出して決着させる。候補:

- **乱数 + 衝突検出（IPv6 SLAAC / mDNS 流）**: 16-bit ほどの空間から
  乱数で自選し、SWIM gossip（[[decentralized-lookup.md]] の `swim.c`）で
  「この ID は私」と主張。衝突を観測したら片方が引き直す（tie-break は
  例えば MAC/公開鍵のハッシュ大小で決定的に）。256 空間では誕生日問題で
  衝突確率が高いので、内部 ID 空間を広げる前提とセット。
- **HRW で「名前 → ID」を決める**: ノードの安定識別子（公開鍵）を
  rendezvous hashing にかけて ID 空間へ写す。[[decentralized-lookup.md]]
  が責任集合計算に使う HRW をそのまま同一性発行に流用でき、状態ゼロ
  （**HRW/min-id 補題は decentralized-lookup.md §2.1–2.2 が正準**；ここは流用のみ）。
  ただし写像衝突（2 つの鍵が同じ ID に落ちる）は必ず起きるので、結局
  衝突検出 + 再試行が要る。

- 長所: 恒久的中央が原理的に不在。relay が死んでも ID 発行が止まらない。
  思想に最も忠実。
- 短所: 収束が gossip 速度に律速され「churn 中はずれる窓」が開く
  （[[decentralized-lookup.md]] §の churn 窓と同じ性質）。実装が重い
  （主張・検出・tie-break・再割当のステートマシン）。現行の 8-bit ID
  空間（`NODE_MAX=256`, drpc は `DNODE_MAX`）では乱数自選は窮屈で、
  ID 空間拡張が前提条件になる（G7 とも絡む）。

### 3C. 結論（今波の判断）

**今波は 3A（relay lease）を実装し、3B（完全 P2P）を Phase D の本丸として
道筋だけ残す。** 理由:

1. 3A は既存資産（relay の node 表）に乗るので**安全な第一歩**として
   小さく、後方互換を壊さずに「手動 ID 固定」を外せる。
2. 3B は ID 空間拡張（G7）と SWIM ベースの主張プロトコルを要し、
   regions/Phase D の自己編成と同時に設計するのが筋。今 3B を中途半端に
   入れると、3A も 3B も無い不安定状態を作る。
3. **思想的な正直さ**: 3A は「relay という一点」を ID 発行に使う。これは
   完全な中央なしではない。だが (a) relay は冗長化済みで、(b) lease の
   権威は「その relay に REGISTER しているクラスタ」局所にとどまり、(c)
   ID は揮発（idle で回収）するので恒久固定でない——「一時的な役割」で
   あって「恒久的中央」ではない、という [[decentralized-lookup.md]] の
   coordinator 論法と同じ立場に立つ。最終形（3B）への通過点と明示する。

将来 3A→3B は**併存可能**: `PKERNEL_NODE_ID` 明示=固定、`=0`=relay lease、
さらに将来 `=p2p`=自選、という三段の取得モードにできる（§5）。

## 4. 複数 relay での lease 調停（設計のみ・未実装）

[[relay-ha.md]] では relay は状態を共有しない。同じクラスタの 2 ノードが
別々の生存 relay に auto-REGISTER すると、各 relay が独立に「最小空き=1」を
貸し、**同じ ID を二重発行**しうる。解決の選択肢:

- **(i) lease 範囲の relay 別パーティション**: relay#k は `[k*S, (k+1)*S)`
  の ID だけ貸す。状態共有不要・衝突原理排除。短所: ノードが relay 間を
  failover すると ID が変わる（relay-ha の failover/failback と非両立——
  ID 安定性を捨てる）。
- **(ii) lease を全 relay へ gossip**: 発行した ID を他 relay へ広告。
  relay 間通信が要る（relay-ha の「relay 同士は通信しない」を破る）。
- **(iii) ID を addr/鍵から決定的に導く**: relay は「貸す」のではなく
  「source tuple を ID 空間へ写す純関数」を全 relay 同一に持つ。どの
  relay でも同じ入力に同じ ID。これは事実上 3B（HRW 自選）を relay 側で
  代行する形で、3A と 3B の橋渡しになる。衝突検出だけ別途要る。

**推奨道筋**: 単一 relay の lease（今波）→ failover で ID を保つため (iii)
の決定的写像へ寄せる → 最終的に写像をノード側へ移して 3B（relay 非依存）。
今波の実装は単一 relay 前提と明記する。

## 5. 後方互換と移行パス

| `PKERNEL_NODE_ID` | 挙動 | 状態 |
|---|---|---|
| 明示（1..255） | 従来どおり固定 ID で REGISTER（`src=node_id`） | 既存・不変 |
| 未指定 / `0` / `auto` | relay へ `src=0` REGISTER、lease された ID を採用 | 今波 relay 側のみ実装。クライアント採用は次波 |
| `p2p`（将来） | SWIM 上で自選 + 衝突検出（3B） | Phase D |

既存の固定 ID 経路（`run_relay_failover.sh` / `run_3node_full.sh` 等）は
一切変わらない。relay は `src>=1` の REGISTER を従来どおり処理し、lease
ロジックは `src==0` のときだけ発火する。

## 6. wire 互換の取り方（実装の要点）

新パケット型を足さず、**既存の `REL_REGISTER` の予約値を使う**:

- `src==0` は v1/v2 とも常に invalid で、`update()`/`replay_check` が
  黙って落としていた値（`relay.c` の範囲チェック）。つまり既存クライアント
  は誰も使っていない。これを **「auto-lease 要求」の特殊値**に充てる。
- **要求**: クライアントが `REGISTER, src=0, dst=0` を送る。v2 の HMAC は
  `src=0` を含めて正しく計算できる（鍵保持の証明＝認証は維持）。
- **付与（grant）**: relay が `REGISTER, src=0, dst=<leased id>` を要求元
  へ返す。`src=0` が「これは grant であって他ノードの REGISTER ではない」
  目印。v2 では relay が鍵で署名するのでクライアントは ID を信頼できる。
  `dst=0` の grant は「プール枯渇＝拒否」。
- **replay**: `src==0` の REGISTER は per-src replay window を持てない
  （ID 未確定）。HMAC で認証済み・送信元 addr で idempotent 化するので、
  この 1 ケースだけ src-keyed replay check をバイパスする。他の全パケット
  （DATA/KEEPALIVE/固定 REGISTER）は従来どおり replay 窓を通る。
- **解放**: 既存の idle eviction を流用。leased slot が無通信で枯れたら
  `active=0` にして回収、ID は再利用可能になる。テスト用に
  `PKERNEL_RELAY_IDLE`（秒）で窓を短縮できる（本番は 300 s 既定のまま）。

ヘッダもパケット型も増えないので `make -C relay test` の 6 本は green の
まま。他ノードの REGISTER（`src>=1`）にも無影響。

## 7. 今波で実装したこと / 残したこと（正直に）

**実装（relay 側のみ）**:
- `relay/relay.c`: `src=0` auto-REGISTER → 最小空き ID の lease + 署名付き
  grant 応答。送信元 addr による idempotent 化（再送/replay で二重発行
  しない）。固定 ID とのプール共有・衝突回避（active slot を skip）。
  idle eviction による回収・再利用。`PKERNEL_RELAY_IDLE` でのテスト用
  窓短縮。
- `samples/19_dynamic_id/`: relay を起動し擬似クライアントで
  ①ユニーク貸与 ②固定 ID 混在で非衝突 ③切断後の回収・再利用 を
  アサート（失敗時 exit 非 0）。

**follow-up / Phase D に残したこと（明示）**:
- **クライアント配線（次波）**: `arch/linux/*/net_relay.c` が
  `PKERNEL_NODE_ID` 未指定時に `src=0` を送り、grant の ID を採用して
  以降それで REGISTER/DATA する処理。今波は relay の挙動のみ検証。
- **複数 relay の lease 調停（§4）**: 設計のみ。実装は単一 relay 前提。
- **完全 P2P 自選（3B）**: ID 空間拡張（G7 連動）＋ SWIM 主張プロトコル。
  Phase D / regions の自己編成と同時設計。
- **自己編成トポロジ（§の relay-ha 先頭優先の中央性解消）**: §4(iii) の
  決定的写像でリスト先頭依存を薄める道筋まで。実コードは将来。
- **ID 安定性 vs failover の両立**: relay 跨ぎで ID を保つ仕組み（§4）。

## 8. 検証

```
make -C relay && make -C relay test   # 既存 6/6 green（後方互換）
samples/19_dynamic_id/run_dynamic_id.sh          # lease/回収/非衝突 PASS
```

固定 ID 経路（`run_relay_failover.sh` / `run_3node_full.sh`）は無影響。
