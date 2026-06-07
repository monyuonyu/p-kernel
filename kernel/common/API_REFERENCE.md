# T-Kernel Common Module API リファレンス

このドキュメントは、T-Kernelの共通モジュール（kernel/common）で提供される全APIの包括的なリファレンスです。

## 目次

1. [タスク管理API](#タスク管理api)
2. [メモリ管理API](#メモリ管理api)
3. [同期オブジェクトAPI](#同期オブジェクトapi)
4. [ロック機能API](#ロック機能api)
5. [デバイス管理API](#デバイス管理api)
6. [タイマー・時間管理API](#タイマー時間管理api)
7. [システム機能API](#システム機能api)
8. [ユーティリティAPI](#ユーティリティapi)
9. [T-MonitorAPI](#t-monitorapi)
10. [デバッグサポートAPI](#デバッグサポートapi)

---

## タスク管理API

### task.c - タスク制御機能

#### `knl_task_initialize()`
```c
ER knl_task_initialize(void)
```
**目的**: タスク制御ブロック（TCB）の初期化  
**戻り値**: 
- `E_OK`: 成功
- `E_SYS`: システムエラー

**説明**: タスク管理システム全体を初期化します。システム起動時に一度だけ呼び出されます。

#### `knl_make_dormant()`
```c
void knl_make_dormant(TCB *tcb)
```
**目的**: タスクを休止状態に設定  
**パラメータ**:
- `tcb`: 対象タスクの制御ブロック

**説明**: タスクを休止状態（DORMANT）に設定し、実行開始に必要な初期化を行います。

#### `knl_make_ready()`
```c
void knl_make_ready(TCB *tcb)
```
**目的**: タスクを実行可能状態に設定  
**パラメータ**:
- `tcb`: 対象タスクの制御ブロック

**説明**: タスク状態をREADYに更新し、実行可能キューに挿入します。

#### `knl_make_non_ready()`
```c
void knl_make_non_ready(TCB *tcb)
```
**目的**: タスクを実行不可能状態に設定  
**パラメータ**:
- `tcb`: 対象タスクの制御ブロック

**説明**: タスクを実行可能キューから削除し、必要に応じて再スケジューリングを行います。

#### `knl_change_task_priority()`
```c
void knl_change_task_priority(TCB *tcb, INT priority)
```
**目的**: タスク優先度の動的変更  
**パラメータ**:
- `tcb`: 対象タスクの制御ブロック
- `priority`: 新しい優先度

**説明**: 実行時にタスクの優先度を変更し、必要に応じてキューの再配置を行います。

#### `knl_rotate_ready_queue()`
```c
void knl_rotate_ready_queue(INT priority)
```
**目的**: 実行可能キューのローテーション  
**パラメータ**:
- `priority`: ローテーション対象の優先度

**説明**: 指定優先度のキューをローテーションし、ラウンドロビンスケジューリングを実現します。

#### `knl_rotate_ready_queue_run()`
```c
void knl_rotate_ready_queue_run(void)
```
**目的**: 最高優先度タスクを含むキューのローテーション

**説明**: 現在の最高優先度レベルでのタスク切り替えを実現します。

### task_manage.c - タスク管理機能

#### `tk_cre_tsk_impl()`
```c
SYSCALL ID tk_cre_tsk_impl(CONST T_CTSK *pk_ctsk)
```
**目的**: タスクの生成  
**パラメータ**:
- `pk_ctsk`: タスク生成情報

**戻り値**: 
- 正値: タスクID
- `E_NOMEM`: メモリ不足
- `E_LIMIT`: リソース不足

#### `tk_del_tsk_impl()`
```c
SYSCALL ER tk_del_tsk_impl(ID tskid)
```
**目的**: タスクの削除  
**パラメータ**:
- `tskid`: 削除するタスクID

**戻り値**: 
- `E_OK`: 成功
- `E_ID`: 不正なID
- `E_OBJ`: オブジェクト状態エラー

#### `tk_sta_tsk_impl()`
```c
SYSCALL ER tk_sta_tsk_impl(ID tskid, INT stacd)
```
**目的**: タスクの実行開始  
**パラメータ**:
- `tskid`: 開始するタスクID
- `stacd`: 開始コード

#### `tk_ext_tsk_impl()`
```c
SYSCALL void tk_ext_tsk_impl(void)
```
**目的**: 自タスクの実行終了

**説明**: 呼び出したタスク自身を終了させます。

#### `tk_exd_tsk_impl()`
```c
SYSCALL void tk_exd_tsk_impl(void)
```
**目的**: 自タスクの実行終了と削除

#### `tk_ter_tsk_impl()`
```c
SYSCALL ER tk_ter_tsk_impl(ID tskid)
```
**目的**: 他タスクの強制終了  
**パラメータ**:
- `tskid`: 終了させるタスクID

#### `tk_chg_pri_impl()`
```c
SYSCALL ER tk_chg_pri_impl(ID tskid, PRI tskpri)
```
**目的**: タスク優先度の変更  
**パラメータ**:
- `tskid`: 対象タスクID
- `tskpri`: 新しい優先度

#### `tk_rot_rdq_impl()`
```c
SYSCALL ER tk_rot_rdq_impl(PRI tskpri)
```
**目的**: 実行可能キューのローテーション  
**パラメータ**:
- `tskpri`: 対象優先度

#### `tk_rel_wai_impl()`
```c
SYSCALL ER tk_rel_wai_impl(ID tskid)
```
**目的**: 待ち状態の強制解除  
**パラメータ**:
- `tskid`: 対象タスクID

#### `tk_get_tid_impl()`
```c
SYSCALL ID tk_get_tid_impl(void)
```
**目的**: 実行中タスクIDの取得  
**戻り値**: 現在実行中のタスクID

#### `tk_ref_tsk_impl()`
```c
SYSCALL ER tk_ref_tsk_impl(ID tskid, T_RTSK *pk_rtsk)
```
**目的**: タスク状態の参照  
**パラメータ**:
- `tskid`: 参照するタスクID
- `pk_rtsk`: タスク状態情報格納先

#### `tk_def_tex_impl()`
```c
SYSCALL ER tk_def_tex_impl(ID tskid, CONST T_DTEX *pk_dtex)
```
**目的**: タスク例外ハンドラの定義  
**パラメータ**:
- `tskid`: 対象タスクID
- `pk_dtex`: 例外ハンドラ定義情報

#### `tk_ena_tex_impl()`
```c
SYSCALL ER tk_ena_tex_impl(ID tskid, UINT texptn)
```
**目的**: タスク例外の許可  
**パラメータ**:
- `tskid`: 対象タスクID
- `texptn`: 許可する例外パターン

#### `tk_dis_tex_impl()`
```c
SYSCALL ER tk_dis_tex_impl(ID tskid, UINT *p_texptn)
```
**目的**: タスク例外の禁止  
**パラメータ**:
- `tskid`: 対象タスクID
- `p_texptn`: 現在の例外パターン格納先

#### `tk_ras_tex_impl()`
```c
SYSCALL ER tk_ras_tex_impl(ID tskid, INT texcd)
```
**目的**: タスク例外の発生  
**パラメータ**:
- `tskid`: 対象タスクID
- `texcd`: 例外コード

#### `tk_end_tex_impl()`
```c
SYSCALL ER tk_end_tex_impl(INT texcd)
```
**目的**: タスク例外ハンドラの終了  
**パラメータ**:
- `texcd`: 例外処理結果

#### `tk_ref_tex_impl()`
```c
SYSCALL ER tk_ref_tex_impl(ID tskid, T_RTEX *pk_rtex)
```
**目的**: タスク例外状態の参照  
**パラメータ**:
- `tskid`: 参照するタスクID
- `pk_rtex`: 例外状態情報格納先

### task_sync.c - タスク同期機能

#### `tk_sus_tsk_impl()`
```c
SYSCALL ER tk_sus_tsk_impl(ID tskid)
```
**目的**: タスクのサスペンド  
**パラメータ**:
- `tskid`: サスペンドするタスクID

#### `tk_rsm_tsk_impl()`
```c
SYSCALL ER tk_rsm_tsk_impl(ID tskid)
```
**目的**: タスクのレジューム  
**パラメータ**:
- `tskid`: レジュームするタスクID

#### `tk_frsm_tsk_impl()`
```c
SYSCALL ER tk_frsm_tsk_impl(ID tskid)
```
**目的**: タスクの強制レジューム  
**パラメータ**:
- `tskid`: 強制レジュームするタスクID

#### `tk_slp_tsk_impl()`
```c
SYSCALL ER tk_slp_tsk_impl(TMO tmout)
```
**目的**: 自タスクのスリープ  
**パラメータ**:
- `tmout`: タイムアウト時間

#### `tk_wup_tsk_impl()`
```c
SYSCALL ER tk_wup_tsk_impl(ID tskid)
```
**目的**: タスクのウェイクアップ  
**パラメータ**:
- `tskid`: ウェイクアップするタスクID

#### `tk_can_wup_impl()`
```c
SYSCALL INT tk_can_wup_impl(ID tskid)
```
**目的**: ウェイクアップ要求のキャンセル  
**パラメータ**:
- `tskid`: 対象タスクID

**戻り値**: キャンセルされたウェイクアップ要求数

#### `tk_dly_tsk_impl()`
```c
SYSCALL ER tk_dly_tsk_impl(RELTIM dlytim)
```
**目的**: 自タスクの遅延実行  
**パラメータ**:
- `dlytim`: 遅延時間（相対時間）

---

## メモリ管理API

### memory.c - 内部メモリ管理

#### `knl_Imalloc()`
```c
void* knl_Imalloc(SZ size)
```
**目的**: 内部メモリの動的割り当て  
**パラメータ**:
- `size`: 割り当てサイズ

**戻り値**: 割り当てられたメモリへのポインタ、失敗時はNULL

#### `knl_Icalloc()`
```c
void* knl_Icalloc(SZ nmemb, SZ size)
```
**目的**: ゼロクリアされた内部メモリの割り当て  
**パラメータ**:
- `nmemb`: 要素数
- `size`: 各要素のサイズ

#### `knl_Irealloc()`
```c
void* knl_Irealloc(void *ptr, SZ size)
```
**目的**: 内部メモリの再割り当て  
**パラメータ**:
- `ptr`: 既存のメモリポインタ
- `size`: 新しいサイズ

#### `knl_Ifree()`
```c
void knl_Ifree(void *ptr)
```
**目的**: 内部メモリの解放  
**パラメータ**:
- `ptr`: 解放するメモリポインタ

### mempfix.c - 固定長メモリプール

#### `tk_cre_mpf_impl()`
```c
SYSCALL ID tk_cre_mpf_impl(CONST T_CMPF *pk_cmpf)
```
**目的**: 固定長メモリプールの生成  
**パラメータ**:
- `pk_cmpf`: メモリプール生成情報

**戻り値**: 
- 正値: メモリプールID
- `E_NOMEM`: メモリ不足

#### `tk_del_mpf_impl()`
```c
SYSCALL ER tk_del_mpf_impl(ID mpfid)
```
**目的**: 固定長メモリプールの削除  
**パラメータ**:
- `mpfid`: メモリプールID

#### `tk_get_mpf_impl()`
```c
SYSCALL ER tk_get_mpf_impl(ID mpfid, void **p_blk, TMO tmout)
```
**目的**: 固定長メモリブロックの取得  
**パラメータ**:
- `mpfid`: メモリプールID
- `p_blk`: 取得したブロックのポインタ格納先
- `tmout`: タイムアウト時間

#### `tk_rel_mpf_impl()`
```c
SYSCALL ER tk_rel_mpf_impl(ID mpfid, void *blk)
```
**目的**: 固定長メモリブロックの返却  
**パラメータ**:
- `mpfid`: メモリプールID
- `blk`: 返却するブロック

#### `tk_ref_mpf_impl()`
```c
SYSCALL ER tk_ref_mpf_impl(ID mpfid, T_RMPF *pk_rmpf)
```
**目的**: 固定長メモリプール状態の参照  
**パラメータ**:
- `mpfid`: メモリプールID
- `pk_rmpf`: 状態情報格納先

### mempool.c - 可変長メモリプール

#### `tk_cre_mpl_impl()`
```c
SYSCALL ID tk_cre_mpl_impl(CONST T_CMPL *pk_cmpl)
```
**目的**: 可変長メモリプールの生成  
**パラメータ**:
- `pk_cmpl`: メモリプール生成情報

#### `tk_del_mpl_impl()`
```c
SYSCALL ER tk_del_mpl_impl(ID mplid)
```
**目的**: 可変長メモリプールの削除  
**パラメータ**:
- `mplid`: メモリプールID

#### `tk_get_mpl_impl()`
```c
SYSCALL ER tk_get_mpl_impl(ID mplid, SZ blksz, void **p_blk, TMO tmout)
```
**目的**: 可変長メモリブロックの取得  
**パラメータ**:
- `mplid`: メモリプールID
- `blksz`: 要求ブロックサイズ
- `p_blk`: 取得したブロックのポインタ格納先
- `tmout`: タイムアウト時間

#### `tk_rel_mpl_impl()`
```c
SYSCALL ER tk_rel_mpl_impl(ID mplid, void *blk)
```
**目的**: 可変長メモリブロックの返却  
**パラメータ**:
- `mplid`: メモリプールID
- `blk`: 返却するブロック

#### `tk_ref_mpl_impl()`
```c
SYSCALL ER tk_ref_mpl_impl(ID mplid, T_RMPL *pk_rmpl)
```
**目的**: 可変長メモリプール状態の参照  
**パラメータ**:
- `mplid`: メモリプールID
- `pk_rmpl`: 状態情報格納先

---

## 同期オブジェクトAPI

### semaphore.c - セマフォ

#### `tk_cre_sem_impl()`
```c
SYSCALL ID tk_cre_sem_impl(CONST T_CSEM *pk_csem)
```
**目的**: セマフォの生成  
**パラメータ**:
- `pk_csem`: セマフォ生成情報

#### `tk_del_sem_impl()`
```c
SYSCALL ER tk_del_sem_impl(ID semid)
```
**目的**: セマフォの削除  
**パラメータ**:
- `semid`: セマフォID

#### `tk_sig_sem_impl()`
```c
SYSCALL ER tk_sig_sem_impl(ID semid, INT cnt)
```
**目的**: セマフォ資源の返却（シグナル）  
**パラメータ**:
- `semid`: セマフォID
- `cnt`: 返却する資源数

#### `tk_wai_sem_impl()`
```c
SYSCALL ER tk_wai_sem_impl(ID semid, INT cnt, TMO tmout)
```
**目的**: セマフォ資源の取得（ウェイト）  
**パラメータ**:
- `semid`: セマフォID
- `cnt`: 要求する資源数
- `tmout`: タイムアウト時間

#### `tk_ref_sem_impl()`
```c
SYSCALL ER tk_ref_sem_impl(ID semid, T_RSEM *pk_rsem)
```
**目的**: セマフォ状態の参照  
**パラメータ**:
- `semid`: セマフォID
- `pk_rsem`: 状態情報格納先

### mutex.c - ミューテックス

#### `tk_cre_mtx_impl()`
```c
SYSCALL ID tk_cre_mtx_impl(CONST T_CMTX *pk_cmtx)
```
**目的**: ミューテックスの生成  
**パラメータ**:
- `pk_cmtx`: ミューテックス生成情報

#### `tk_del_mtx_impl()`
```c
SYSCALL ER tk_del_mtx_impl(ID mtxid)
```
**目的**: ミューテックスの削除  
**パラメータ**:
- `mtxid`: ミューテックスID

#### `tk_loc_mtx_impl()`
```c
SYSCALL ER tk_loc_mtx_impl(ID mtxid, TMO tmout)
```
**目的**: ミューテックスのロック  
**パラメータ**:
- `mtxid`: ミューテックスID
- `tmout`: タイムアウト時間

#### `tk_unl_mtx_impl()`
```c
SYSCALL ER tk_unl_mtx_impl(ID mtxid)
```
**目的**: ミューテックスのアンロック  
**パラメータ**:
- `mtxid`: ミューテックスID

#### `tk_ref_mtx_impl()`
```c
SYSCALL ER tk_ref_mtx_impl(ID mtxid, T_RMTX *pk_rmtx)
```
**目的**: ミューテックス状態の参照  
**パラメータ**:
- `mtxid`: ミューテックスID
- `pk_rmtx`: 状態情報格納先

### eventflag.c - イベントフラグ

#### `tk_cre_flg_impl()`
```c
SYSCALL ID tk_cre_flg_impl(CONST T_CFLG *pk_cflg)
```
**目的**: イベントフラグの生成  
**パラメータ**:
- `pk_cflg`: イベントフラグ生成情報

#### `tk_del_flg_impl()`
```c
SYSCALL ER tk_del_flg_impl(ID flgid)
```
**目的**: イベントフラグの削除  
**パラメータ**:
- `flgid`: イベントフラグID

#### `tk_set_flg_impl()`
```c
SYSCALL ER tk_set_flg_impl(ID flgid, UINT setptn)
```
**目的**: イベントフラグのセット  
**パラメータ**:
- `flgid`: イベントフラグID
- `setptn`: セットするビットパターン

#### `tk_clr_flg_impl()`
```c
SYSCALL ER tk_clr_flg_impl(ID flgid, UINT clrptn)
```
**目的**: イベントフラグのクリア  
**パラメータ**:
- `flgid`: イベントフラグID
- `clrptn`: クリアするビットパターン

#### `tk_wai_flg_impl()`
```c
SYSCALL ER tk_wai_flg_impl(ID flgid, UINT waiptn, UINT wfmode, UINT *p_flgptn, TMO tmout)
```
**目的**: イベントフラグの待ち  
**パラメータ**:
- `flgid`: イベントフラグID
- `waiptn`: 待ちビットパターン
- `wfmode`: 待ちモード（AND/OR、クリア条件）
- `p_flgptn`: 取得したビットパターン格納先
- `tmout`: タイムアウト時間

#### `tk_ref_flg_impl()`
```c
SYSCALL ER tk_ref_flg_impl(ID flgid, T_RFLG *pk_rflg)
```
**目的**: イベントフラグ状態の参照  
**パラメータ**:
- `flgid`: イベントフラグID
- `pk_rflg`: 状態情報格納先

### mailbox.c - メールボックス

#### `tk_cre_mbx_impl()`
```c
SYSCALL ID tk_cre_mbx_impl(CONST T_CMBX *pk_cmbx)
```
**目的**: メールボックスの生成  
**パラメータ**:
- `pk_cmbx`: メールボックス生成情報

#### `tk_del_mbx_impl()`
```c
SYSCALL ER tk_del_mbx_impl(ID mbxid)
```
**目的**: メールボックスの削除  
**パラメータ**:
- `mbxid`: メールボックスID

#### `tk_snd_mbx_impl()`
```c
SYSCALL ER tk_snd_mbx_impl(ID mbxid, T_MSG *pk_msg)
```
**目的**: メールボックスへのメッセージ送信  
**パラメータ**:
- `mbxid`: メールボックスID
- `pk_msg`: 送信するメッセージ

#### `tk_rcv_mbx_impl()`
```c
SYSCALL ER tk_rcv_mbx_impl(ID mbxid, T_MSG **ppk_msg, TMO tmout)
```
**目的**: メールボックスからのメッセージ受信  
**パラメータ**:
- `mbxid`: メールボックスID
- `ppk_msg`: 受信メッセージポインタ格納先
- `tmout`: タイムアウト時間

#### `tk_ref_mbx_impl()`
```c
SYSCALL ER tk_ref_mbx_impl(ID mbxid, T_RMBX *pk_rmbx)
```
**目的**: メールボックス状態の参照  
**パラメータ**:
- `mbxid`: メールボックスID
- `pk_rmbx`: 状態情報格納先

### messagebuf.c - メッセージバッファ

#### `tk_cre_mbf_impl()`
```c
SYSCALL ID tk_cre_mbf_impl(CONST T_CMBF *pk_cmbf)
```
**目的**: メッセージバッファの生成  
**パラメータ**:
- `pk_cmbf`: メッセージバッファ生成情報

#### `tk_del_mbf_impl()`
```c
SYSCALL ER tk_del_mbf_impl(ID mbfid)
```
**目的**: メッセージバッファの削除  
**パラメータ**:
- `mbfid`: メッセージバッファID

#### `tk_snd_mbf_impl()`
```c
SYSCALL ER tk_snd_mbf_impl(ID mbfid, CONST void *msg, SZ msgsz, TMO tmout)
```
**目的**: メッセージバッファへのメッセージ送信  
**パラメータ**:
- `mbfid`: メッセージバッファID
- `msg`: 送信データ
- `msgsz`: メッセージサイズ
- `tmout`: タイムアウト時間

#### `tk_rcv_mbf_impl()`
```c
SYSCALL INT tk_rcv_mbf_impl(ID mbfid, void *msg, TMO tmout)
```
**目的**: メッセージバッファからのメッセージ受信  
**パラメータ**:
- `mbfid`: メッセージバッファID
- `msg`: 受信データ格納先
- `tmout`: タイムアウト時間

**戻り値**: 受信したメッセージサイズ

#### `tk_ref_mbf_impl()`
```c
SYSCALL ER tk_ref_mbf_impl(ID mbfid, T_RMBF *pk_rmbf)
```
**目的**: メッセージバッファ状態の参照  
**パラメータ**:
- `mbfid`: メッセージバッファID
- `pk_rmbf`: 状態情報格納先

### rendezvous.c - ランデブー

#### `tk_cre_por_impl()`
```c
SYSCALL ID tk_cre_por_impl(CONST T_CPOR *pk_cpor)
```
**目的**: ランデブーポートの生成  
**パラメータ**:
- `pk_cpor`: ランデブーポート生成情報

#### `tk_del_por_impl()`
```c
SYSCALL ER tk_del_por_impl(ID porid)
```
**目的**: ランデブーポートの削除  
**パラメータ**:
- `porid`: ランデブーポートID

#### `tk_cal_por_impl()`
```c
SYSCALL INT tk_cal_por_impl(ID porid, UINT calptn, void *msg, SZ cmsgsz, TMO tmout)
```
**目的**: ランデブーの呼出し  
**パラメータ**:
- `porid`: ランデブーポートID
- `calptn`: 呼出しパターン
- `msg`: メッセージ
- `cmsgsz`: 呼出しメッセージサイズ
- `tmout`: タイムアウト時間

**戻り値**: 応答メッセージサイズ

#### `tk_acp_por_impl()`
```c
SYSCALL INT tk_acp_por_impl(ID porid, UINT acpptn, RNO *p_rdvno, void *msg, TMO tmout)
```
**目的**: ランデブーの受付  
**パラメータ**:
- `porid`: ランデブーポートID
- `acpptn`: 受付パターン
- `p_rdvno`: ランデブー番号格納先
- `msg`: メッセージ格納先
- `tmout`: タイムアウト時間

**戻り値**: 受信メッセージサイズ

#### `tk_fwd_por_impl()`
```c
SYSCALL ER tk_fwd_por_impl(ID porid, UINT calptn, RNO rdvno, void *msg, SZ cmsgsz)
```
**目的**: ランデブーの転送  
**パラメータ**:
- `porid`: 転送先ランデブーポートID
- `calptn`: 呼出しパターン
- `rdvno`: ランデブー番号
- `msg`: メッセージ
- `cmsgsz`: 呼出しメッセージサイズ

#### `tk_rpl_rdv_impl()`
```c
SYSCALL ER tk_rpl_rdv_impl(RNO rdvno, void *msg, SZ rmsgsz)
```
**目的**: ランデブーの応答  
**パラメータ**:
- `rdvno`: ランデブー番号
- `msg`: 応答メッセージ
- `rmsgsz`: 応答メッセージサイズ

#### `tk_ref_por_impl()`
```c
SYSCALL ER tk_ref_por_impl(ID porid, T_RPOR *pk_rpor)
```
**目的**: ランデブーポート状態の参照  
**パラメータ**:
- `porid`: ランデブーポートID
- `pk_rpor`: 状態情報格納先

---

## ロック機能API

### fastlock.c - 高速排他制御ロック

#### `CreateLock()`
```c
ER CreateLock(FastLock *lock, CONST UB *name)
```
**目的**: 高速ロックの生成  
**パラメータ**:
- `lock`: 初期化するロックオブジェクト
- `name`: オブジェクト名（NULL可）

#### `DeleteLock()`
```c
ER DeleteLock(FastLock *lock)
```
**目的**: 高速ロックの削除  
**パラメータ**:
- `lock`: 削除するロックオブジェクト

#### `Lock()`
```c
ER Lock(FastLock *lock)
```
**目的**: ロック取得（無限待ち）  
**パラメータ**:
- `lock`: ロックオブジェクト

#### `LockTmo()`
```c
ER LockTmo(FastLock *lock, TMO tmo)
```
**目的**: タイムアウト指定付きロック取得  
**パラメータ**:
- `lock`: ロックオブジェクト
- `tmo`: タイムアウト時間

#### `Unlock()`
```c
ER Unlock(FastLock *lock)
```
**目的**: ロック解放  
**パラメータ**:
- `lock`: ロックオブジェクト

### fastmlock.c - 高速マルチロック

#### `CreateMLock()`
```c
ER CreateMLock(FastMLock *lock, CONST UB *name)
```
**目的**: マルチロックオブジェクトの生成  
**パラメータ**:
- `lock`: 初期化するマルチロックオブジェクト
- `name`: オブジェクト名（NULL可）

#### `DeleteMLock()`
```c
ER DeleteMLock(FastMLock *lock)
```
**目的**: マルチロックオブジェクトの削除  
**パラメータ**:
- `lock`: 削除するマルチロックオブジェクト

#### `MLock()`
```c
ER MLock(FastMLock *lock, INT no)
```
**目的**: ロック取得（無限待ち）  
**パラメータ**:
- `lock`: マルチロックオブジェクト
- `no`: ロック番号（0-31）

#### `MLockTmo()`
```c
ER MLockTmo(FastMLock *lock, INT no, TMO tmo)
```
**目的**: タイムアウト指定付きロック取得  
**パラメータ**:
- `lock`: マルチロックオブジェクト
- `no`: ロック番号（0-31）
- `tmo`: タイムアウト時間

#### `MUnlock()`
```c
ER MUnlock(FastMLock *lock, INT no)
```
**目的**: ロック解放  
**パラメータ**:
- `lock`: マルチロックオブジェクト
- `no`: ロック番号（0-31）

### klock.c - カーネルロック

#### `knl_LockOBJ()`
```c
void knl_LockOBJ(OBJLOCK *loc)
```
**目的**: オブジェクトロック取得  
**パラメータ**:
- `loc`: ロックオブジェクト

**注意**: クリティカルセクションからは呼び出さないでください

#### `knl_UnlockOBJ()`
```c
void knl_UnlockOBJ(OBJLOCK *loc)
```
**目的**: オブジェクトロック解放  
**パラメータ**:
- `loc`: ロックオブジェクト

**注意**: クリティカルセクションからも安全に呼び出せます

---

## デバイス管理API

### device.c - デバイス管理

#### `tk_def_dev_impl()`
```c
SYSCALL ID tk_def_dev_impl(CONST UB *devnm, CONST T_DDEV *pk_ddev, T_IDEV *pk_idev)
```
**目的**: デバイスドライバの登録  
**パラメータ**:
- `devnm`: デバイス名
- `pk_ddev`: デバイス定義情報
- `pk_idev`: デバイス初期設定情報

#### `tk_ref_idv_impl()`
```c
SYSCALL ER tk_ref_idv_impl(T_IDEV *pk_idev)
```
**目的**: デバイス初期情報の参照  
**パラメータ**:
- `pk_idev`: 初期情報格納先

#### `tk_get_dev_impl()`
```c
SYSCALL ID tk_get_dev_impl(ID devid, UB *devnm)
```
**目的**: デバイス名の取得  
**パラメータ**:
- `devid`: デバイスID
- `devnm`: デバイス名格納先

#### `tk_ref_dev_impl()`
```c
SYSCALL ID tk_ref_dev_impl(CONST UB *devnm, T_RDEV *pk_rdev)
```
**目的**: デバイス情報の取得  
**パラメータ**:
- `devnm`: デバイス名
- `pk_rdev`: デバイス情報格納先

#### `tk_oref_dev_impl()`
```c
SYSCALL ID tk_oref_dev_impl(ID dd, T_RDEV *pk_rdev)
```
**目的**: オープン済みデバイスの情報取得  
**パラメータ**:
- `dd`: デバイスディスクリプタ
- `pk_rdev`: デバイス情報格納先

#### `tk_lst_dev_impl()`
```c
SYSCALL INT tk_lst_dev_impl(T_LDEV *pk_ldev, INT start, INT ndev)
```
**目的**: 登録デバイス一覧の取得  
**パラメータ**:
- `pk_ldev`: デバイス一覧格納先
- `start`: 開始位置
- `ndev`: 取得数

#### `tk_evt_dev_impl()`
```c
SYSCALL INT tk_evt_dev_impl(ID devid, INT evttyp, void *evtinf)
```
**目的**: デバイスイベントの送信  
**パラメータ**:
- `devid`: デバイスID
- `evttyp`: イベントタイプ
- `evtinf`: イベント情報

### deviceio.c - デバイス入出力制御

#### `tk_opn_dev_impl()`
```c
SYSCALL ID tk_opn_dev_impl(CONST UB *devnm, UINT omode)
```
**目的**: デバイスのオープン  
**パラメータ**:
- `devnm`: デバイス名
- `omode`: オープンモード

**戻り値**: デバイスディスクリプタ

#### `tk_cls_dev_impl()`
```c
SYSCALL ER tk_cls_dev_impl(ID dd, UINT option)
```
**目的**: デバイスのクローズ  
**パラメータ**:
- `dd`: デバイスディスクリプタ
- `option`: クローズオプション

#### `tk_rea_dev_impl()`
```c
SYSCALL ER tk_rea_dev_impl(ID dd, INT start, void *buf, SZ size, TMO tmout)
```
**目的**: デバイスからの読み込み  
**パラメータ**:
- `dd`: デバイスディスクリプタ
- `start`: 読み込み開始位置
- `buf`: データ格納先
- `size`: 読み込みサイズ
- `tmout`: タイムアウト時間

#### `tk_wri_dev_impl()`
```c
SYSCALL ER tk_wri_dev_impl(ID dd, INT start, CONST void *buf, SZ size, TMO tmout)
```
**目的**: デバイスへの書き込み  
**パラメータ**:
- `dd`: デバイスディスクリプタ
- `start`: 書き込み開始位置
- `buf`: 書き込みデータ
- `size`: 書き込みサイズ
- `tmout`: タイムアウト時間

#### `tk_wai_dev_impl()`
```c
SYSCALL INT tk_wai_dev_impl(ID dd, ID reqid, INT *asize, ER *ioer, TMO tmout)
```
**目的**: I/O完了待ち  
**パラメータ**:
- `dd`: デバイスディスクリプタ
- `reqid`: 要求ID
- `asize`: 実際の処理サイズ格納先
- `ioer`: I/Oエラー格納先
- `tmout`: タイムアウト時間

#### `tk_sus_dev_impl()`
```c
SYSCALL ER tk_sus_dev_impl(UINT susmode)
```
**目的**: デバイスサスペンド  
**パラメータ**:
- `susmode`: サスペンドモード

---

## タイマー・時間管理API

### timer.c - タイマー制御

#### `knl_timer_insert()`
```c
void knl_timer_insert(TMEB *tmeb, TMO tmout, CBACK callback, void *arg)
```
**目的**: タイマーイベントの登録  
**パラメータ**:
- `tmeb`: タイマーイベントブロック
- `tmout`: タイムアウト時間
- `callback`: コールバック関数
- `arg`: コールバック引数

#### `knl_timer_insert_reltim()`
```c
void knl_timer_insert_reltim(TMEB *tmeb, RELTIM tmout, CBACK callback, void *arg)
```
**目的**: 相対時間指定でのタイマーイベント登録  
**パラメータ**:
- `tmeb`: タイマーイベントブロック
- `tmout`: 相対タイムアウト時間
- `callback`: コールバック関数
- `arg`: コールバック引数

#### `knl_timer_delete()`
```c
void knl_timer_delete(TMEB *tmeb)
```
**目的**: タイマーイベントの削除  
**パラメータ**:
- `tmeb`: タイマーイベントブロック

### time_calls.c - 時間関連システムコール

#### `tk_set_tim_impl()`
```c
SYSCALL ER tk_set_tim_impl(CONST SYSTIM *pk_tim)
```
**目的**: システム時刻の設定  
**パラメータ**:
- `pk_tim`: 設定する時刻

#### `tk_get_tim_impl()`
```c
SYSCALL ER tk_get_tim_impl(SYSTIM *pk_tim)
```
**目的**: システム時刻の取得  
**パラメータ**:
- `pk_tim`: 時刻格納先

#### `tk_get_otm_impl()`
```c
SYSCALL ER tk_get_otm_impl(SYSTIM *pk_tim)
```
**目的**: 稼働時間の取得  
**パラメータ**:
- `pk_tim`: 稼働時間格納先

#### `tk_cre_cyc_impl()`
```c
SYSCALL ID tk_cre_cyc_impl(CONST T_CCYC *pk_ccyc)
```
**目的**: 周期ハンドラの生成  
**パラメータ**:
- `pk_ccyc`: 周期ハンドラ生成情報

#### `tk_del_cyc_impl()`
```c
SYSCALL ER tk_del_cyc_impl(ID cycid)
```
**目的**: 周期ハンドラの削除  
**パラメータ**:
- `cycid`: 周期ハンドラID

#### `tk_sta_cyc_impl()`
```c
SYSCALL ER tk_sta_cyc_impl(ID cycid)
```
**目的**: 周期ハンドラの動作開始  
**パラメータ**:
- `cycid`: 周期ハンドラID

#### `tk_stp_cyc_impl()`
```c
SYSCALL ER tk_stp_cyc_impl(ID cycid)
```
**目的**: 周期ハンドラの動作停止  
**パラメータ**:
- `cycid`: 周期ハンドラID

#### `tk_ref_cyc_impl()`
```c
SYSCALL ER tk_ref_cyc_impl(ID cycid, T_RCYC *pk_rcyc)
```
**目的**: 周期ハンドラ状態の参照  
**パラメータ**:
- `cycid`: 周期ハンドラID
- `pk_rcyc`: 状態情報格納先

#### `tk_cre_alm_impl()`
```c
SYSCALL ID tk_cre_alm_impl(CONST T_CALM *pk_calm)
```
**目的**: アラームハンドラの生成  
**パラメータ**:
- `pk_calm`: アラームハンドラ生成情報

#### `tk_del_alm_impl()`
```c
SYSCALL ER tk_del_alm_impl(ID almid)
```
**目的**: アラームハンドラの削除  
**パラメータ**:
- `almid`: アラームハンドラID

#### `tk_sta_alm_impl()`
```c
SYSCALL ER tk_sta_alm_impl(ID almid, RELTIM almtim)
```
**目的**: アラームハンドラの動作開始  
**パラメータ**:
- `almid`: アラームハンドラID
- `almtim`: アラーム時間

#### `tk_stp_alm_impl()`
```c
SYSCALL ER tk_stp_alm_impl(ID almid)
```
**目的**: アラームハンドラの動作停止  
**パラメータ**:
- `almid`: アラームハンドラID

#### `tk_ref_alm_impl()`
```c
SYSCALL ER tk_ref_alm_impl(ID almid, T_RALM *pk_ralm)
```
**目的**: アラームハンドラ状態の参照  
**パラメータ**:
- `almid`: アラームハンドラID
- `pk_ralm`: 状態情報格納先

---

## システム機能API

### misc_calls.c - その他システムコール

#### `tk_ref_sys_impl()`
```c
SYSCALL ER tk_ref_sys_impl(T_RSYS *pk_rsys)
```
**目的**: システム状態の参照  
**パラメータ**:
- `pk_rsys`: システム状態情報格納先

#### `tk_ref_ver_impl()`
```c
SYSCALL ER tk_ref_ver_impl(T_RVER *pk_rver)
```
**目的**: カーネルバージョン情報の取得  
**パラメータ**:
- `pk_rver`: バージョン情報格納先

#### `tk_ena_dsp_impl()`
```c
SYSCALL ER tk_ena_dsp_impl(void)
```
**目的**: タスクディスパッチの許可

#### `tk_dis_dsp_impl()`
```c
SYSCALL ER tk_dis_dsp_impl(void)
```
**目的**: タスクディスパッチの禁止

### wait.c - 同期処理共通ルーチン

#### `knl_wait_release_ok()`
```c
void knl_wait_release_ok(TCB *tcb)
```
**目的**: 正常終了での待ち解除  
**パラメータ**:
- `tcb`: 対象タスクの制御ブロック

#### `knl_wait_release_ok_ercd()`
```c
void knl_wait_release_ok_ercd(TCB *tcb, ER ercd)
```
**目的**: 指定エラーコードでの待ち解除  
**パラメータ**:
- `tcb`: 対象タスクの制御ブロック
- `ercd`: 設定するエラーコード

#### `knl_wait_release_ng()`
```c
void knl_wait_release_ng(TCB *tcb, ER ercd)
```
**目的**: エラー終了での待ち解除  
**パラメータ**:
- `tcb`: 対象タスクの制御ブロック
- `ercd`: 設定するエラーコード

#### `knl_wait_release_tmout()`
```c
void knl_wait_release_tmout(TCB *tcb)
```
**目的**: タイムアウトによる待ち解除  
**パラメータ**:
- `tcb`: 対象タスクの制御ブロック

#### `knl_make_wait()`
```c
void knl_make_wait(TMO tmout, ATR atr)
```
**目的**: アクティブタスクを待ち状態に変更  
**パラメータ**:
- `tmout`: タイムアウト時間
- `atr`: 待ち属性

#### `knl_make_wait_reltim()`
```c
void knl_make_wait_reltim(RELTIM tmout, ATR atr)
```
**目的**: 相対時間指定でアクティブタスクを待ち状態に変更  
**パラメータ**:
- `tmout`: 相対タイムアウト時間
- `atr`: 待ち属性

#### `knl_wait_delete()`
```c
void knl_wait_delete(QUEUE *wait_queue)
```
**目的**: 待ちキューの全タスクをE_DLTエラーで解除  
**パラメータ**:
- `wait_queue`: 対象の待ちキュー

#### `knl_wait_tskid()`
```c
ID knl_wait_tskid(QUEUE *wait_queue)
```
**目的**: 待ちキューの先頭タスクIDを取得  
**パラメータ**:
- `wait_queue`: 対象の待ちキュー

**戻り値**: 先頭タスクのID、キューが空の場合は0

#### `knl_gcb_make_wait()`
```c
void knl_gcb_make_wait(GCB *gcb, TMO tmout)
```
**目的**: 汎用制御ブロック（GCB）用の待ち状態設定  
**パラメータ**:
- `gcb`: 汎用制御ブロック
- `tmout`: タイムアウト時間

#### `knl_gcb_change_priority()`
```c
void knl_gcb_change_priority(GCB *gcb, TCB *tcb)
```
**目的**: タスク優先度変更時の待ちキュー位置調整  
**パラメータ**:
- `gcb`: 汎用制御ブロック
- `tcb`: 優先度が変更されたタスクの制御ブロック

#### `knl_gcb_top_of_wait_queue()`
```c
TCB* knl_gcb_top_of_wait_queue(GCB *gcb, TCB *tcb)
```
**目的**: 待ちキューの第一候補タスクの検索  
**パラメータ**:
- `gcb`: 汎用制御ブロック
- `tcb`: 検索対象に含めるタスクの制御ブロック

**戻り値**: 最初に実行されるべきタスクの制御ブロック

### tkstart.c - カーネル起動・終了

#### `knl_t_kernel_main()`
```c
INT knl_t_kernel_main(void)
```
**目的**: T-Kernelのメイン処理

#### `knl_t_kernel_exit()`
```c
void knl_t_kernel_exit(void)
```
**目的**: T-Kernelの終了処理

### subsystem.c - サブシステム管理

#### `tk_def_ssy_impl()`
```c
SYSCALL ID tk_def_ssy_impl(ID ssid, CONST T_DSSY *pk_dssy)
```
**目的**: サブシステムの定義  
**パラメータ**:
- `ssid`: サブシステムID
- `pk_dssy`: サブシステム定義情報

#### `tk_ref_ssy_impl()`
```c
SYSCALL ER tk_ref_ssy_impl(ID ssid, T_RSSY *pk_rssy)
```
**目的**: サブシステム状態の参照  
**パラメータ**:
- `ssid`: サブシステムID
- `pk_rssy`: 状態情報格納先

### objname.c - オブジェクト名管理

#### `knl_object_getname()`
```c
ER knl_object_getname(UINT objtype, ID objid, UB **name)
```
**目的**: オブジェクト名の取得  
**パラメータ**:
- `objtype`: オブジェクトタイプ
- `objid`: オブジェクトID
- `name`: 名前格納先

#### `knl_object_setname()`
```c
ER knl_object_setname(UINT objtype, ID objid, CONST UB *name)
```
**目的**: オブジェクト名の設定  
**パラメータ**:
- `objtype`: オブジェクトタイプ
- `objid`: オブジェクトID
- `name`: 設定する名前

---

## ユーティリティAPI

### bitop.c - ビット操作ライブラリ

#### `knl_tstdlib_bitclr()`
```c
void knl_tstdlib_bitclr(void *base, W offset)
```
**目的**: 指定位置のビットをクリア（0に設定）  
**パラメータ**:
- `base`: ビット列の開始アドレス
- `offset`: クリアするビットの位置（0から開始）

#### `knl_tstdlib_bitset()`
```c
void knl_tstdlib_bitset(void *base, W offset)
```
**目的**: 指定位置のビットをセット（1に設定）  
**パラメータ**:
- `base`: ビット列の開始アドレス
- `offset`: セットするビットの位置（0から開始）

#### `knl_tstdlib_bitsearch1()`
```c
W knl_tstdlib_bitsearch1(void *base, W offset, W width)
```
**目的**: ビット列から1が設定されているビットを検索  
**パラメータ**:
- `base`: ビット列の開始アドレス
- `offset`: 検索開始位置（0から開始）
- `width`: 検索範囲の幅（ビット数）

**戻り値**: 最初に見つかった1ビットの位置、見つからない場合は-1

---

## T-MonitorAPI

### tm_monitor.c - T-Monitorメイン

#### `knl_tm_main()`
```c
INT knl_tm_main(void)
```
**目的**: T-Monitorのメイン処理

#### `knl_tm_exit()`
```c
void knl_tm_exit(INT status)
```
**目的**: T-Monitorの終了  
**パラメータ**:
- `status`: 終了ステータス

### tm_printf.c - 書式付き文字列出力

#### `knl_tm_printf()`
```c
INT knl_tm_printf(CONST char *format, ...)
```
**目的**: 書式付き文字列の出力  
**パラメータ**:
- `format`: 書式文字列
- `...`: 可変個引数

**戻り値**: 出力した文字数

#### `knl_tm_sprintf()`
```c
INT knl_tm_sprintf(char *str, CONST char *format, ...)
```
**目的**: 書式付き文字列の生成  
**パラメータ**:
- `str`: 格納先文字列
- `format`: 書式文字列
- `...`: 可変個引数

### tm_getchar.c / tm_putchar.c - 文字入出力

#### `knl_tm_getchar()`
```c
INT knl_tm_getchar(void)
```
**目的**: 1文字の入力  
**戻り値**: 入力された文字、エラー時はEOF

#### `knl_tm_putchar()`
```c
INT knl_tm_putchar(INT c)
```
**目的**: 1文字の出力  
**パラメータ**:
- `c`: 出力する文字

### tm_getline.c / tm_putstring.c - 行・文字列入出力

#### `knl_tm_getline()`
```c
INT knl_tm_getline(char *line)
```
**目的**: 1行の入力  
**パラメータ**:
- `line`: 入力文字列格納先

#### `knl_tm_putstring()`
```c
INT knl_tm_putstring(CONST char *str)
```
**目的**: 文字列の出力  
**パラメータ**:
- `str`: 出力する文字列

### tm_command.c - コマンド処理

#### `knl_tm_command()`
```c
INT knl_tm_command(char *line)
```
**目的**: コマンドの実行  
**パラメータ**:
- `line`: コマンド文字列

---

## デバッグサポートAPI

各モジュールは、`USE_DBGSPT`が有効な場合にデバッグサポート関数（`td_*`関数）を提供します。これらの関数は主にデバッガやシステム監視ツールで使用されます。

### 主要なデバッグサポート関数

#### `td_rdy_que_impl()`
```c
SYSCALL INT td_rdy_que_impl(PRI pri, ID list[], INT nent)
```
**目的**: 実行可能キューの参照  
**パラメータ**:
- `pri`: 参照する優先度
- `list`: タスクIDを格納する配列
- `nent`: 配列の要素数

**戻り値**: 指定優先度の実行可能タスク数

---

## 使用例

### セマフォの基本的な使用例

```c
#include <tkernel.h>

ID semid;

// セマフォの生成
T_CSEM csem = {
    NULL,           // exinf
    TA_TFIFO,      // sematr
    1,             // isemcnt (初期カウント)
    1              // maxsem (最大カウント)
};

semid = tk_cre_sem(&csem);
if (semid < E_OK) {
    // エラー処理
    return semid;
}

// セマフォの取得
ER ercd = tk_wai_sem(semid, 1, TMO_FEVR);
if (ercd != E_OK) {
    // エラー処理
    return ercd;
}

// クリティカルセクション
// ...

// セマフォの返却
tk_sig_sem(semid, 1);

// セマフォの削除
tk_del_sem(semid);
```

### タスクの生成と実行例

```c
#include <tkernel.h>

// タスクエントリ関数
void task_main(INT stacd, void *exinf)
{
    tm_printf("Task started with code: %d\n", stacd);
    
    while (1) {
        // タスクの処理
        tk_dly_tsk(1000); // 1秒待機
    }
}

// タスクの生成と開始
ID tskid;
T_CTSK ctsk = {
    NULL,                    // exinf
    TA_HLNG | TA_RNG0,      // tskatr
    (FP)task_main,          // task
    3,                      // itskpri
    1024,                   // stksz
    NULL                    // bufptr
};

tskid = tk_cre_tsk(&ctsk);
if (tskid < E_OK) {
    return tskid;
}

// タスクの開始
ER ercd = tk_sta_tsk(tskid, 0);
if (ercd != E_OK) {
    tk_del_tsk(tskid);
    return ercd;
}
```

### メモリプールの使用例

```c
#include <tkernel.h>

ID mpfid;
void *blk;

// 固定長メモリプールの生成
T_CMPF cmpf = {
    NULL,           // exinf
    TA_TFIFO,      // mpfatr
    10,            // mpfcnt (ブロック数)
    64,            // blfsz (ブロックサイズ)
    NULL           // bufptr
};

mpfid = tk_cre_mpf(&cmpf);
if (mpfid < E_OK) {
    return mpfid;
}

// メモリブロックの取得
ER ercd = tk_get_mpf(mpfid, &blk, TMO_FEVR);
if (ercd == E_OK) {
    // メモリブロックの使用
    memset(blk, 0, 64);
    
    // メモリブロックの返却
    tk_rel_mpf(mpfid, blk);
}

// メモリプールの削除
tk_del_mpf(mpfid);
```

---

## エラーコード

### 主要なエラーコード

| エラーコード | 意味 |
|-------------|------|
| `E_OK` | 正常終了 |
| `E_SYS` | システムエラー |
| `E_NOMEM` | メモリ不足 |
| `E_NOSPT` | 未サポート機能 |
| `E_INOSPT` | 未サポート機能（実装依存） |
| `E_RSATR` | 予約属性 |
| `E_PAR` | パラメータエラー |
| `E_ID` | 不正ID番号 |
| `E_CTX` | コンテキストエラー |
| `E_MACV` | メモリアクセス違反 |
| `E_OACV` | オブジェクトアクセス違反 |
| `E_ILUSE` | サービスコール不正使用 |
| `E_NOMEM` | メモリ不足 |
| `E_LIMIT` | リソース不足 |
| `E_OBJ` | オブジェクト状態エラー |
| `E_NOEXS` | オブジェクト未登録 |
| `E_QOVR` | キューオーバーフロー |
| `E_RLWAI` | 待ち状態の強制解除 |
| `E_TMOUT` | ポーリング失敗またはタイムアウト |
| `E_DLT` | オブジェクト削除待ち解除 |
| `E_DISWAI` | 待ち禁止による待ち解除 |

---

## 注意事項

1. **システムコール**: `SYSCALL`で定義された関数は、アプリケーションから直接呼び出すことができるシステムコールです。

2. **内部関数**: `EXPORT`で定義された関数は、カーネル内部で使用される関数で、通常はアプリケーションから直接呼び出しません。

3. **タイムアウト**: `TMO_FEVR`（永久待ち）または`TMO_POL`（ポーリング）、あるいは具体的な時間値を指定できます。

4. **リアルタイム性**: T-Kernelはリアルタイムオペレーティングシステムです。優先度の高いタスクが常に優先されます。

5. **割り込み**: 一部の関数は割り込み禁止区間で実行されます。長時間の処理は避けてください。

---

このAPIリファレンスは、T-Kernel Common Moduleの主要な機能を網羅しています。詳細な実装については、各ソースファイルのドキュメントコメントを参照してください。