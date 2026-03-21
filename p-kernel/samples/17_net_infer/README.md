# ステップ6: AI 推論 + UDP ネットワーク配信

センサーデータを AI で分類し、その結果を UDP ネットワーク経由で配信するサンプルです。
p-kernel の AI syscall とネットワーク syscall を組み合わせた、最も発展的なサンプルです。

## 学べること

- **ローカル MLP 推論** (`sys_infer`) — センサー値をカーネル内の AI モデルで即時分類
- **非同期 AI ジョブ** (`sys_ai_submit` / `sys_ai_wait`) — バックグラウンドで AI 処理を実行
- **UDP バインド** (`sys_udp_bind`) — ローカルポートをリッスン登録
- **UDP 送信** (`sys_udp_send`) — センサーアラートをネットワーク経由で配信
- **UDP 受信** (`sys_udp_recv`) — タイムアウト付きでパケットを待機

## ソースファイル

```
06_net_infer/
└── net_infer.c    3 フェーズのデモ (推論 → 非同期ジョブ → UDP 送受信)
```

## ビルドと実行

```bash
# ビルド
cd userland/x86
make 06_net_infer/net_infer.elf

# ディスクイメージに格納して QEMU 起動
cd boot/x86
make disk
make run-disk

# シェルから実行
p-kernel> exec net_infer.elf
```

## 期待する出力

```
========================================
 net_infer: AI 推論 + UDP 配信デモ
========================================

--- Phase 1: ローカル MLP 推論 (SYS_INFER) ---

  No  温度  湿度  気圧   照度  分類      シナリオ
  --  ----  ----  -----  ----  --------  ------------------------
  1   25C  55%  1013hPa  500lx  normal    室温・通常運転
  2   45C  60%  1013hPa  500lx  normal    高温 (夏場の屋内)
  3   70C  80%  950hPa  100lx  CRITICAL  高温・多湿・低気圧・暗所
  4   20C  20%  1013hPa  500lx  normal    低湿度 (乾燥注意)
  5   60C  65%  980hPa  200lx  alert     高温・低気圧 (複合異常)

--- Phase 2: 非同期 AI ジョブ (SYS_AI_SUBMIT / SYS_AI_WAIT) ---

  [submit]
    job[0] handle=0  (normal ...)
    job[1] handle=1  (alert ...)
    job[2] handle=2  (critical ...)
  [wait & result]
    job[0] -> normal
    job[1] -> normal
    job[2] -> CRITICAL

--- Phase 3: UDP 送受信 (SYS_UDP_BIND / SEND / RECV) ---

  udp_bind(9000): OK
  udp_send to 10.0.2.2:8888: queued (ARP pending)
  udp_recv(port=9000, timeout=2000ms): timeout (expected in single-node mode)

========================================
 net_infer: done
========================================
[proc] exited (code=0)
```

## AI モデルの仕組み

p-kernel のカーネルには **4→8→8→3 MLP ニューラルネットワーク**が組み込まれています。

```
入力 (int8 Q8, 正規化済み):
  temp     = (°C  - 20) × 2    例: 70°C → 100
  humidity = (%   - 50) × 2    例: 80%  → 60
  pressure = (hPa-1013) / 2    例: 950  → -32
  light    = (lux- 500) / 4    例: 100  → -100
     ↓
  [Layer1: 8 units, ReLU]  各入力の逸脱を検出
     ↓
  [Layer2: 8 units, ReLU]  複合異常を集約
     ↓
  出力クラス (argmax):
    0 = normal    (すべて正常範囲内)
    1 = alert     (1〜2 項目が軽度逸脱)
    2 = CRITICAL  (複数項目が重度逸脱)
```

## センサー値の正規化 (int8 Q8)

センサーの生の物理値を `[-127, 127]` の int8 に変換してから渡します。

```c
int packed = SYS_SENSOR_PACK(
    norm_temp(70),      // (70-20)*2 = 100
    norm_hum(80),       // (80-50)*2 = 60
    norm_press(950),    // (950-1013)/2 = -32
    norm_light(100)     // (100-500)/4 = -100
);
int cls = sys_infer(packed);  // → 2 (CRITICAL)
```

## 同期 vs 非同期推論

| 方式 | syscall | 特徴 |
|------|---------|------|
| 同期推論 | `sys_infer(packed)` | 呼び出し元タスクで即時実行。最も簡単。 |
| 非同期ジョブ | `sys_ai_submit(packed)` + `sys_ai_wait(handle, tmo)` | AI ワーカータスクに委譲。複数ジョブを並行投入できる。 |

## UDP syscall 一覧

| syscall | 説明 |
|---------|------|
| `sys_udp_bind(port)` | ローカルポートにバインド。受信パケットはカーネル内バッファに蓄積される。 |
| `sys_udp_send(&pk)` | UDP データグラムを送信。ARP 未解決の場合は -1 (要リトライ)。 |
| `sys_udp_recv(&pk)` | パケット到着を待つ。`timeout_ms=-1` で永遠待ち、正の値でタイムアウト。 |

## 2 ノード構成での完全デモ

ネットワーク送受信を実際に動かすには 2 つの QEMU インスタンスが必要です。

```bash
# terminal 0: node0 (10.1.0.1)
make run-node0

# terminal 1: node1 (10.1.0.2)
make run-node1

# node0 で送信したパケットを node1 が受信できます
[node0] p-kernel> exec net_infer.elf
[node1] p-kernel> exec net_infer.elf
```

## 次のステップ

```
p-kernel> exec all_demo.elf   ← POSIX/タスク/セマフォの全機能テスト (OK=59 NG=0)
```
