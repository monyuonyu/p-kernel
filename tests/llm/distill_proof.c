/*
 *  distill_proof.c — MINIMAL, FAST falsifiable proof of the NS-1 distill
 *  MECHANISM (wave-distill-proof). It reuses the SAME student.c API as the
 *  canonical student_test.c, but is bounded for a throttled host: NO full
 *  finite-diff gradcheck (student_test.c already certs that), a small number
 *  of sleep rounds, and explicit train-vs-held-out reporting.
 *
 *  What it proves (or falsifies):
 *    - the fresh baby starts near chance ln(256) on HELD-OUT data,
 *    - after K real sleep rounds on the TEACHER's bytes the HELD-OUT loss
 *      MEASURABLY drops (generalisation, not memorisation),
 *    - a SCRAMBLED-teacher control (random bytes, identical #updates) does NOT
 *      lower held-out loss on the real corpus.
 *
 *  It prints REAL NUMBERS (train loss, held-out loss, deltas, wall-time) and
 *  exits 0 only if the headline drop is real AND beats the control. Falsifiable.
 *
 *  Usage: distill_proof <fixture.bytes> [rounds] [lr] [seqlen]
 *  Build: cc -std=c11 -O1 -ffp-contract=off distill_proof.c ../../arch/common/llm/student.c -o distill_proof
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../arch/common/llm/student.h"

static uint8_t *g_corpus = NULL;
static int      g_corpus_n = 0;

static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000.0+t.tv_nsec/1e6; }

static void window(uint8_t *dst, int off, int len){
    for (int i=0;i<len;i++) dst[i]=g_corpus[(off+i)%g_corpus_n];
}

/* mean held-out loss over `count` windows starting at train_end */
static float heldout_loss(st_model *m, int seqlen, int train_end, int count){
    uint8_t buf[ST_MAXSEQ]; double s=0; int got=0;
    for (int w=0;w<count;w++){
        window(buf, train_end + w*seqlen, seqlen);
        int np=0; float l=st_eval_loss(m,buf,seqlen,&np);
        if(np){ s+=l; got++; }
    }
    return got?(float)(s/got):0.0f;
}

/* mean train loss over the train windows */
static float train_loss(st_model *m, int seqlen, int train_windows){
    uint8_t buf[ST_MAXSEQ]; double s=0; int got=0;
    for (int w=0;w<train_windows;w++){
        window(buf, w*seqlen, seqlen);
        int np=0; float l=st_eval_loss(m,buf,seqlen,&np);
        if(np){ s+=l; got++; }
    }
    return got?(float)(s/got):0.0f;
}

static void sleep_rounds(st_model *m,int seqlen,int train_windows,int rounds,float lr,int scramble){
    uint8_t buf[ST_MAXSEQ]; uint32_t rng=0x5EED1234u;
    float *logits=(float*)malloc((size_t)seqlen*ST_VOCAB*sizeof(float));
    for(int r=0;r<rounds;r++){
        for(int w=0;w<train_windows;w++){
            if(scramble){ for(int i=0;i<seqlen;i++){ rng=rng*1664525u+1013904223u; buf[i]=(uint8_t)((rng>>16)&0xff);} }
            else window(buf, w*seqlen, seqlen);
            st_zero_grad(m);
            st_forward(m,buf,seqlen,logits);
            st_backward(m,buf,seqlen);
            st_adam_step(m,lr);
        }
    }
    free(logits);
}

int main(int argc,char**argv){
    const char *fx = (argc>1)?argv[1]:"tests/llm/student_teacher.bytes";
    int rounds  = (argc>2)?atoi(argv[2]):8;
    float lr    = (argc>3)?(float)atof(argv[3]):3e-3f;
    int seqlen  = (argc>4)?atoi(argv[4]):32;
    if(seqlen<2||seqlen>ST_MAXSEQ) seqlen=32;

    FILE *f=fopen(fx,"rb");
    if(!f){ fprintf(stderr,"cannot open fixture %s\n",fx); return 2; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<seqlen*4){ fprintf(stderr,"fixture too small (%ld)\n",sz); fclose(f); return 2; }
    g_corpus=(uint8_t*)malloc(sz); if(fread(g_corpus,1,sz,f)!=(size_t)sz){fclose(f);return 2;} fclose(f);
    g_corpus_n=(int)sz;

    int total = g_corpus_n/seqlen;
    int trainw = total*3/4; if(trainw<2) trainw=2;
    int heldw  = total-trainw; if(heldw<1) heldw=1;
    int train_end = trainw*seqlen;
    float chance = st_logf(256.0f);

    printf("=== distill_proof (FAST mechanism cert) ===\n");
    printf("fixture=%s bytes=%d seqlen=%d train=%dwin held=%dwin rounds=%d lr=%.4f chance=%.4f nats\n",
           fx,g_corpus_n,seqlen,trainw,heldw,rounds,lr,chance);

    /* ---- real teaching ---- */
    st_model m; if(st_init(&m,0x0BABE)!=ST_OK){fprintf(stderr,"OOM\n");return 1;}
    float pre_held = heldout_loss(&m,seqlen,train_end,heldw);
    float pre_tr   = train_loss(&m,seqlen,trainw);
    printf("\n[honest-baby]   pre  train=%.4f  held-out=%.4f  (chance %.4f)\n",pre_tr,pre_held,chance);
    double t0=now_ms();
    sleep_rounds(&m,seqlen,trainw,rounds,lr,0);
    double t1=now_ms();
    float post_held=heldout_loss(&m,seqlen,train_end,heldw);
    float post_tr  =train_loss(&m,seqlen,trainw);
    printf("[distill]       post train=%.4f  held-out=%.4f  (%.0f ms)\n",post_tr,post_held,t1-t0);
    printf("                train drop=%.4f  HELD-OUT drop=%.4f (%.1f%% of chance)\n",
           pre_tr-post_tr, pre_held-post_held, 100.0f*(pre_held-post_held)/chance);
    st_free(&m);

    /* ---- scrambled control ---- */
    st_model ms; st_init(&ms,0x0BABE);
    float spre=heldout_loss(&ms,seqlen,train_end,heldw);
    sleep_rounds(&ms,seqlen,trainw,rounds,lr,1);
    float spost=heldout_loss(&ms,seqlen,train_end,heldw);
    printf("[control-rand]  held-out %.4f -> %.4f  (drop %.4f)\n",spre,spost,spre-spost);
    st_free(&ms);

    float real_drop = pre_held-post_held;
    float ctrl_drop = spre-spost;
    int honest_ok = (pre_held > chance-0.30f);
    int drop_ok   = (post_held < pre_held-0.20f);
    int grounded_ok = (ctrl_drop < real_drop*0.5f) && (spost > chance-0.30f);
    printf("\nVERDICT: honest=%d drop=%d grounded=%d  (real_drop=%.4f ctrl_drop=%.4f)\n",
           honest_ok,drop_ok,grounded_ok,real_drop,ctrl_drop);
    int ok = honest_ok && drop_ok && grounded_ok;
    printf("%s\n", ok?"PASS — distill mechanism PROVEN on held-out data":"FAIL — held-out gain not real / not grounded");
    free(g_corpus);
    return ok?0:1;
}
