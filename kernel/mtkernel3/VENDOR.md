# μT-Kernel 3.0 取り込み元情報（vendoring provenance）

このディレクトリは TRON フォーラムが公開している μT-Kernel 3.0
（mtkernel_3）のソースコードを取り込んだものです。

- 取り込み元: https://github.com/tron-forum/mtkernel_3
- コミット: `435096c96136c847774b5d6de07cc092b1398778`（master, 2024-04-01）
- 取り込み日: 2026-07-01
- ライセンス: T-License 2.2（`docs/TEF000-219-200401.pdf` を同梱）
- 除外したもの: `build_make/`（IDE 向けビルドファイル）、`device/`
  （サンプルデバイスドライバ）、`docs/`（ライセンス PDF 以外）、`ucode.png`

p-kernel への移植に伴う変更（sysdepend の追加・コンフィグ調整など）は、
この取り込みコミットより後のコミットで行い、上流との差分が git 履歴で
追跡できるようにしています。
