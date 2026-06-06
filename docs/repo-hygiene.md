# Repository hygiene — HEAD cleanup (2026-06-06)

PR #3 の指摘への対応記録。対象は **HEAD のクリーンアップのみ**。
履歴の書き換え (git filter-repo) は明示的にスコープ外とし、後述の通り保留する。

> 指摘: Intel の IA-32 マニュアル PDF 4冊・`.bin`/`.elf`/`.pcap` バイナリ・
> `tryos` ベンダリングをコミット。肥大化。`p-kernel/p-kernel/` の入れ子パスも扱いづらい。

## 分類基準

| 分類 | 意味 | 処置 |
|---|---|---|
| (a) | リポジトリに不要な参照資料 | 削除 |
| (b) | gitignore すべきビルド/実行成果物 | 削除 + .gitignore 追加 |
| (c) | ランタイム/ビルドが実際に参照するバイナリ | 保持(理由を記録) |
| (d) | 判断がつかないもの | 保持 + 本書に列挙(オーナー判断待ち) |

## インベントリ(追跡中の 300KB 超ファイル + 全バイナリ種)

| パス | サイズ (bytes) | 分類 | 処置 |
|---|---:|---|---|
| `p-kernel/arch/x86/IA32_Arh_Dev_Man_Vol3_i.pdf` | 9,900,565 | (a) | 削除 |
| `p-kernel/arch/x86/IA32_Arh_Dev_Man_Vol2A_i.pdf` | 5,376,399 | (a) | 削除 |
| `p-kernel/arch/x86/IA32_Arh_Dev_Man_Vol1_Online_i.pdf` | 5,177,330 | (a) | 削除 |
| `p-kernel/arch/x86/IA32_Arh_Dev_Man_Vol2B_i.pdf` | 4,315,830 | (a) | 削除 |
| `p-kernel/boot/x86/mcast1.pcap` | 42,370 | (b) | 削除 + ignore |
| `p-kernel/boot/x86/mcast0.pcap` | 41,430 | (b) | 削除 + ignore |
| `p-kernel/boot/x86/node0.pcap` | 9,752 | (b) | 削除 + ignore |
| `p-kernel/boot/x86/node1.pcap` | 9,752 | (b) | 削除 + ignore |
| `p-kernel/boot/x86/test_output.log` | 0 | (b) | 削除 + ignore |
| `p-kernel/boot/x86/user_hello/test_all.elf` | 18,972 | (b) | 削除 + ignore |
| `p-kernel/boot/x86/user_hello/demo.elf` | 8,524 | (b) | 削除 + ignore |
| `p-kernel/boot/x86/user_hello/hello.elf` | 4,708 | (b) | 削除 + ignore |
| `p-kernel/arch/x86/tryos/.../PINoC/boot/boot.bin` | 2,560 | (b) | 削除 (`*.bin` は既に ignore 済) |
| `p-kernel/arch/x86/tryos/.../PINoC/boot/bootld.bin` | 2,048 | (b) | 削除 (同上) |
| `p-kernel/arch/x86/tryos/.../PINoC/boot/bootsct.bin` | 512 | (b) | 削除 (同上) |
| `p-kernel/arch/x86/tryos/.../PINoC/Kernel/main.bin` | 1,536 | (b) | 削除 (同上) |
| `p-kernel/arch/x86/tryos/.../PINoC/Kernel/#main.bin#` | 1,536 | (b) | 削除 (emacs バックアップ。`\#*\#` を ignore 追加) |
| `p-kernel/android/gradle/wrapper/gradle-wrapper.jar` | 43,453 | (c) | **保持** — Gradle wrapper の標準構成。`./gradlew` がこれを require する。Gradle 公式もコミットを推奨 |
| `p-kernel/arch/x86/tryos/` (ソース ~35 ファイル, ~52KB) | ~52,000 | (d) | **保持** — 下記参照 |
| `p-kernel/arch/x86/tryos/.../PINoC/boot/tmp` | 5,432 | (d) | **保持** — 中身は C ソースのスクラッチ。tryos ツリーと運命を共にすべき |
| `p-kernel/drivers/h8300/DRAM/.gitkeep` ほか `.gitkeep` ×2 | 0 | (c) | 保持 — ディレクトリ保持用 |

判断根拠(削除前に確認したこと):

- `*.pcap` は QEMU の `-object filter-dump` 実行成果物。Makefile / スクリプトのどこからも参照されていない (grep 済)。
- `user_hello/*.elf` は同ディレクトリの `.c` のビルド成果物。`boot/x86/Makefile` の disk.img 生成は `userland/x86/` 配下の `.elf`(追跡されていない、都度ビルド)だけを mcopy しており、`user_hello/*.elf` を参照するものはない。`samples/*/README.md` が `cd boot/x86/user_hello` に言及するが、ビルド手順の話でありコミット済バイナリは不要。
- IA-32 マニュアル PDF は Intel 公式サイト (Intel SDM, https://www.intel.com/sdm) から入手可能な参照資料。リポジトリに置く理由がない。
- `tryos` 内の `.bin` は同梱ソースから makefile で再生成できるブート artifacts。

## HEAD から削除した合計

**24,913,824 bytes (≈ 23.8 MiB)** — うち PDF 4冊で 24,770,124 bytes (99.4%)。

## 追加した .gitignore

リポジトリの慣習(ディレクトリ単位の .gitignore、例: `p-kernel/boot/linux/.gitignore`)に従う。

- `p-kernel/boot/x86/.gitignore`(新規): `*.pcap`, `test_output.log`, `user_hello/*.elf`
- `p-kernel/arch/x86/.gitignore`(新規): `*.pdf`(参照資料の再コミット防止)
- ルート `.gitignore`(追記): `\#*\#`, `*~`(エディタのバックアップ/一時ファイル)

`*.bin` / `*.o` はルート .gitignore で既に ignore 済(コミット済だった tryos の .bin は追跡が ignore に優先していただけ)。

## 保持したが要オーナー判断 (d)

### `p-kernel/arch/x86/tryos/` — PINoC 実験ツリー (~52KB)

x86 リアルモード→プロテクトモード移行を試した過去の実験 OS (PINoC) のソース一式。
p-kernel 本体のビルドからは一切参照されていない (grep 済)。PR #3 は「ベンダリング」を
指摘するが、肥大化の実体はほぼ PDF だった(tryos ソースは 52KB)。バイナリ artifacts
のみ削除し、ソースは**履歴的価値の判断をオーナーに委ねる**ために残した。
削除するなら `git rm -r p-kernel/arch/x86/tryos` の 1 コマンドで済む。

## `p-kernel/p-kernel/` の入れ子パスについて(現状維持)

リポジトリルート直下に `p-kernel/` ディレクトリがあり、リポジトリ名も p-kernel のため
checkout すると `p-kernel/p-kernel/...` という入れ子パスになる、という指摘。

- **正体**: 歴史的経緯。リポジトリは元々 pinokernel / PCAT_x86 / toolchain などを含む
  「作業場」全体で、その一区画として `p-kernel/` サブツリーが育った。現在はほぼ全ての
  生きたコードが `p-kernel/` 配下にあり、ルートには `docs/`(GitHub Pages)と README しかない。
- **今回触らない理由**: 約 900 ファイルの一斉移動になり、全 Makefile の相対パス
  (`../../arch/...` 等)・CI・各エージェントが並行作業中のブランチ(feat/regions-r0,
  pfs-p1, r3a-train, ...)すべてと衝突する。HEAD クリーンアップの域を超える。
- **将来フラット化するなら**: (1) 並行ブランチが全部 master に合流した静止点を選ぶ、
  (2) `git mv p-kernel/* .` + ルート docs/ との衝突解決(`p-kernel/docs/` と root `docs/` の統合)、
  (3) Makefile の相対 include パスは深さが 1 段減るので `../../` → `../` の総点検、
  (4) 履歴追跡性のため移動だけの単独コミットにする。

## 履歴のパージ(保留 — オーナー判断)

`git count-objects -vH`: **pack 155.64 MiB**(+ loose 8.84 MiB)。
HEAD は今回の削除後 ~10MiB 程度のソースなので、pack の大半は履歴上の削除済バイナリ:

- `toolchain/windows/arm/` — Windows 用 ARM GCC ツールチェーン一式(**6,273 ファイル**。.exe, PDF docs 含む。おそらく pack の大半)
- `pinokernel/`, `PCAT_x86/`(pinoc.img, link.exe, Stirling.exe ほか)
- 今回 HEAD から消した IA-32 PDF 4冊 (~24.8MiB) と各種 .elf/.pcap/.bin

`git filter-repo --path toolchain --path PCAT_x86 --path pinokernel --invert-paths`
(+ PDF・pcap・elf のパス指定)で **155MiB → おそらく 10〜20MiB 台**まで落ちる見込み。
ただし全コミットハッシュが変わるため、open PR・全ブランチ・各エージェントの worktree が
無効化される。**並行作業が落ち着いたタイミングでオーナーが実施を判断すること。**

## ビルド確認(削除後)

| ビルド | 結果 |
|---|---|
| `make -C p-kernel/boot/linux` | [OK] p-kernel built (1,044,144 bytes) |
| `make -C p-kernel/boot/linux_x86_64` | [OK] p-kernel built (2,102,832 bytes) |
| `make -C p-kernel/boot/x86 all` | kernel.elf + kloader.bin リンク成功 (exit 0) |
| `make -C p-kernel/boot/aarch64` | kernel.elf 183,573 text (exit 0) |
