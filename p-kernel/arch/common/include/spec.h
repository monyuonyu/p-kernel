/*
 *  spec.h — R3b "呼吸するパラメータ" (breathing parameters)
 *
 *  道B (survival-network.md §7 / regions R3b): ニューロンはデバイスに宿る
 *  疎なエキスパートとして増える。新デバイス参加 = 新しい小エキスパートが
 *  現れ、ルーターが疎に発火 (加算的); デバイス死 = そのニューロン群消失を
 *  ルーターが迂回 = それ自体が縮退。
 *
 *  監査 §4 への直接の答え: これまで MoE のエキスパート (= ノード) は全員が
 *  同じ 635 パラメータの重みを持っていた = 「混合」ではなく「同一コピーが
 *  N 個」。本モジュールは各エキスパートを *別々に専門分化* させ、参加が
 *  増えると精度が上がり (join → 賢くなる)、減ると優雅に劣化する (leave →
 *  graceful degradation) ことを *数で* 示す。
 *
 *  専門分化の機構 (採用):
 *    (a) データシャード specialization: 専門家 c は自分の担当クラス c を
 *        強く重み付けした訓練集合で学習し、そのクラスに強い検出器になる。
 *    (b) 異なる初期化:        各専門家は別 seed で初期化 (dtr_reinit_weights)
 *        してから学習 → 自然に別解へ収束する。
 *  ルーティングは moe の §7 ゲート (moe_gate_predict) を使い、入力のクラス
 *  帯に対応する専門家を疎に発火させる (専門外は発火しない)。
 *
 *  すべて純ローカル計算 (network/kdds に触れない) — ベアメタルでも走る。
 */

#pragma once
#include "kernel.h"

/* `breathe` シェルコマンドの本体。args は "breathe" の直後を指す。
 *   breathe            — join 上昇 + leave 優雅劣化を走らせ表で示す (self-check)
 *   breathe run        — 同上
 *   breathe save       — 学習済み各専門家を p-fs ref "dtr/expert/<k>" へ保存
 *   breathe stat       — 直近実行の要約 */
void breathe_cmd(const UB *args, UW len);

/* カーネル内 self-test: 専門分化 → join 上昇 / leave 優雅劣化を検証し、
 * 失敗数を返す (0 = PASS)。CI が stdin 経由で `breathe` を叩き PASS 行を
 * grep する (pfs/hrw/moe と同じ方式)。 */
INT  breathe_self_test(void);
