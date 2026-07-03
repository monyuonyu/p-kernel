# μT-Kernel 3.0 取り込み元情報と同期運用（vendoring / fork sync）

このディレクトリは TRON フォーラムが公開している μT-Kernel 3.0
（mtkernel_3）のソースコードを取り込み、p-kernel 向けの変更
（LP64 対応・linux_x86_64 ポート・日本語コメント刷新）を加えたものです。

- 上流: https://github.com/tron-forum/mtkernel_3
- フォーク（開発用）: https://github.com/monyuonyu/mtkernel_3
- 取り込みベース: 上流コミット `435096c96136c847774b5d6de07cc092b1398778`
  （master, 2024-04-01）
- ライセンス: T-License 2.2（`docs/TEF000-219-200401.pdf` を同梱）
- 除外したもの: `build_make/`（IDE 向けビルドファイル）、`device/`
  （サンプルデバイスドライバ）、`docs/`（ライセンス PDF 以外）、`ucode.png`

## 開発の構図

    tron-forum/mtkernel_3（上流）
        └─ monyuonyu/mtkernel_3（フォーク）← カーネル本体の加工はここで行う
              └─ p-kernel の kernel/mtkernel3/ ← git subtree として取り込む

このディレクトリは**フォークのリポジトリルートと 1:1 のレイアウト**を
保っています（p-kernel 独自のポートも上流の sysdepend 規約に沿って
`kernel/sysdepend/linux_x86_64/` 等に配置）。そのため git subtree で
双方向に同期できます。

## フォークへの初回反映（seed）

p-kernel 側の変更履歴だけを抜き出したブランチ `mtk3-export` を
用意してあります（`git subtree split --prefix=kernel/mtkernel3` で生成。
パスはフォークのルート相対）。フォークのクローンで:

```sh
git remote add pkernel https://github.com/monyuonyu/p-kernel.git
git fetch pkernel mtk3-export

# フォークの master は上流 435096c と同一ツリーなので、
# ベース（vendor コミット）を除いた変更コミットがそのまま乗る
git cherry-pick <vendorコミット>..pkernel/mtk3-export
git push origin master        # または作業ブランチへ
```

## 以後の同期

- フォーク → p-kernel（取り込み）:

  ```sh
  git subtree pull --prefix=kernel/mtkernel3 \
      https://github.com/monyuonyu/mtkernel_3.git master --squash
  ```

- p-kernel → フォーク（書き戻し）:

  ```sh
  git subtree push --prefix=kernel/mtkernel3 \
      https://github.com/monyuonyu/mtkernel_3.git <ブランチ名>
  ```

注意: Claude Code のリモートセッションから同期を行う場合は、
セッションのリポジトリアクセスに monyuonyu/mtkernel_3 を追加して
おく必要があります（未許可だと fetch/push が拒否されます）。
