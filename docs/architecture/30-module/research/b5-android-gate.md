# B5 の周を Android で既定に（b5-gc-restart.md §5・b5-m-default.md §3 の③）

2026-11-25 22:45 の回が設計だけ書いた（実装は次の作る回、監査は別の回）。

## 1. 今

- 周は `PKERNEL_B5_ROUND_S`（r3_incontext.c の student_region_task、未設定＝動かない）。PC は起動の台本で入れる。Android の JNI（pkernel_jni.c nativeBoot）は入れていない＝Android では周が無い
- 星（ノード全体）の門は PKernelService.kt の runAllowed()＝電池が床より上か充電中 かつ（Wi-Fi だけの設定なら）従量でない Wi-Fi。止めるのは stopSelf だけ（カーネルを止める口が無い）
- b5-m-default.md §3 ③: Android は**充電中かつ従量でない回線のときだけ**、周は長く（例 21600 秒）

## 2. 決めたこと（おすすめ。他の案は §4）

1. **周の門をカーネルに 1 つ**: hosted に `pk_b5_allow`（既定 1＝PC は今と同じ）。student_region_task は毎秒の見回りで 0 なら何もしない（publish・merge・see とも）。読むのは 1 語（揺れても次の秒に直る）
2. **JNI**: nativeBoot で `PKERNEL_B5_ROUND_S=21600` と `pk_b5_allow=0`（起動時は閉じる）。新しい口 `nativeSetB5Allowed(Boolean)`
3. **Kotlin**: reconcileGate（電池の受け手と回線の callback の両方が通る所）で `充電中 && onUnmeteredWifi()` を毎回送る。**星の門と違い、分からないときは閉じる**（周は回線を食う。星の門の fail-open は止まらないための選択で、目的が逆）。判断は RunGate.kt と同じく純粋な関数に分け、tests/android/rungate の形で単体を足す
4. 周の途中で閉じたら、その gen は飛ばす（publish 済みなら merge だけ欠ける＝今の「断りは言う、この周はもうしない」と同じ扱い）

## 3. 合否と陰性（実装の前に固定）

- A1（hosted、2 台・S・周 120 秒）: 門 0 のあいだ `auto: publish` の行が 0、門 1 にした後の周で 1 以上。門はシェルの口（`baby gate 0|1` の形、名前は実装で既存の動詞に合わせる）で切り替える
- A2（Kotlin 単体）: 充電中×回線（従量でない Wi-Fi・従量・無し・分からない）の 8 通り＝開くのは「充電中かつ従量でない Wi-Fi」の 1 つだけ
- A3: crown（ベアの .text）同じ、hosted の警告 57 のまま、build_all ALL OK
- 陰性: `-DB5_NC_NO_GATE`（門を見ない）→ A1 赤。Kotlin は「分からないを開く」壊しで A2 赤
- 限界（先に書く）: 実機と emulator では確かめられない（この機体に Android の環境が無い。CI の emulator smoke は周を見ない）

## 4. 選ばなかった案

- 星の門（runAllowed）に充電を足す: 電池で動く参加（今の既定）ごと止まる
- env だけで決める（起動時に充電中なら周あり）: 起動後に抜いても周が続く
- 周を短く（1800 秒）: 携帯の回線と電池に重い。PC は 1800 のまま
