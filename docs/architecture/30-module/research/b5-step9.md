# B5 第 9 歩: 結果（判定の種 149〜156）

2026-11-10、実装者の記録（02:05 の回）。設計 b5-step9-design.md の線（0.03）・種・陰性は変えていない。student.c は変えていない（ベアの .text も不変、触ったのは tests/llm だけ）。

## 作ったもの

- student_adopt_test.c: 段 s9a・s9b・s9y・s9c・s9d（第 8 歩の腕そのまま、印は `[b5s9]`）、**s9p**（pair2: 毒の 2 台は作るが誰にも送らない＝受け手は正直な 1 台だけと合体）、**s9r1・s9r2**（でたらめ 2・近い 2 を、毒の乱数の種 ＋7919×rep で）、**s9n**（NC1 のビルドの近い 2、rep 0 だけ）。rep 0 は第 8 歩の種と全バイト同じ式
- 判定役 `tests/llm/b5_step9_check.py`（R1〜R6・R8・R9 は第 8 歩の式、R7′、NC1、読み。自己試験 82）と `run_b5_step9.sh`（CI の外、R7′ に要る段を先に走らせる）
- **pair2 と神様の目（探りの B9_MODE 1、`pk-scratch/b9p/res/out`）は見た種 143・148 で行の数字が全部同じ**: 143 last 4.2782 kept 3.8999 at 13・gate 2.7367・lastgate 3.0427・took 4/8・app 4・tsH 0.303・cHmax 0.584、148 last 3.8932 kept 3.7681 at 16・gate 2.6878・peek 3.7679 at 20・lastgate 2.7667・took 4/8・app 8・tsH 0.349・cHmax 0.708（違うのは tsP・cPmin・cut＝毒がいないので −1・2.000・0）。NC1 のビルドの pair2 も 143 で同じ（正直どうしはこだまにならない）

## 判定

（走行中）
