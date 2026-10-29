------------------------------ MODULE MindRule ------------------------------
(* ROADMAP 1-3m: the 1-3b "answer now" rule of arch/common/r3_incontext.c as
   a small model, checked by TLC over every order of delivery.
   Design and the C map: docs/architecture/30-module/research/mind-rule-tla.md.
   Line numbers below are r3_incontext.c on master 3cd974e9.

   Per node and key one engram (value, stamp, teacher) or none (v = 0).
   The network is a SET of packets: a re-send of a packet still in flight
   changes nothing, a packet is removed when it is received or dropped, and
   re-sends (Republish, PushRedrive) put it back = duplicates. Any order. *)
EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS Nodes,       \* node ids (numbers; the tie goes to the smaller)
          Keys, Vals,  \* Vals >= 1 (0 = not held)
          MaxTeach, MaxDrop, MaxEvict, MaxRestart,
          WithAnswer,  \* pull answers (mq_answer_build 4674) on/off
          Mut,         \* "none" | "M1" | "M2" | "M3" | "M4" (design: break-its)
          BadNode,     \* M3: the node whose tie goes to the larger teacher
          StampMax,    \* bound for the fairness universe only
          Teachers,    \* who may Teach (= Nodes in every run of the design;
                       \* a subset only shrinks the runs, see run_tlc.py)
          DupAdopt,    \* FALSE = the C today (5159 drops a same-value packet
                       \* and keeps its own stamp); TRUE = the candidate fix
          Script       \* <<>> = any teach (every run before 10-30); else the
                       \* teaches <<n, k, v>> in this order, one story's runs
                       \* only (the deliveries, drops and re-sends stay free)

VARIABLES held, clk, last, hist, pub, push, net,
          taught, nTeach, nDrop, nEvict, nRestart,
          seen, p4bad, p5bad   \* ghosts: best (stamp, teacher) ever held; flags

vars == <<held, clk, last, hist, pub, push, net, taught, nTeach, nDrop,
          nEvict, nRestart, seen, p4bad, p5bad>>

None  == [v |-> 0, s |-> 0, t |-> 0]
Max(a, b) == IF a > b THEN a ELSE b
\* the rule (mc_incoming_wins 3463): larger stamp; equal -> smaller teacher
Beats(s1, t1, s2, t2) == s1 > s2 \/ (s1 = s2 /\ t1 < t2)
Pkt(o, k, s, v) == [o |-> o, k |-> k, s |-> s, v |-> v]
SendAll(n, p) == { [to |-> m, o |-> p.o, k |-> p.k, s |-> p.s, v |-> p.v] :
                   m \in Nodes \ {n} }
Rec(k, h) == [k |-> k, v |-> h.v, s |-> h.s, t |-> h.t]

\* n now holds h for k: keep the ghosts (P4: never below the best ever held)
SetHeld(n, k, h) ==
  /\ held' = [held EXCEPT ![n][k] = h]
  /\ p4bad' = (p4bad \/ (seen[n][k].s > 0 /\
                         Beats(seen[n][k].s, seen[n][k].t, h.s, h.t)))
  /\ seen' = IF seen[n][k].s = 0 \/ Beats(h.s, h.t, seen[n][k].s, seen[n][k].t)
             THEN [seen EXCEPT ![n][k] = [s |-> h.s, t |-> h.t]] ELSE seen

Init ==
  /\ held = [n \in Nodes |-> [k \in Keys |-> None]]
  /\ clk = [n \in Nodes |-> 0]
  /\ last = [n \in Nodes |-> [o \in Nodes |-> 0]]   \* mt_last_seq 4935
  /\ hist = [n \in Nodes |-> {}]
  /\ pub = [n \in Nodes |-> {}]                       \* mt_pub_last 3075
  /\ push = [n \in Nodes |-> {}]                      \* mc_push_pkt 3494 (one slot)
  /\ net = {}
  /\ taught = {} /\ nTeach = 0 /\ nDrop = 0 /\ nEvict = 0 /\ nRestart = 0
  /\ seen = [n \in Nodes |-> [k \in Keys |-> [s |-> 0, t |-> 0]]]
  /\ p4bad = FALSE /\ p5bad = FALSE

(* m_teach 3742: the same value held -> no-op (3770); otherwise learn or
   revise with the next local seq, which is the stamp (mc_stamp_of 3390),
   note the old one (mc_note 3789), publish to the region (m_publish_teach). *)
Teach(n, k, v) ==
  /\ n \in Teachers
  /\ nTeach < MaxTeach
  /\ Script = <<>> \/ (nTeach < Len(Script) /\ Script[nTeach + 1] = <<n, k, v>>)
  /\ held[n][k].v # v
  /\ LET s == clk[n] + 1
         p == Pkt(n, k, s, v) IN
     /\ clk' = [clk EXCEPT ![n] = s]
     /\ SetHeld(n, k, [v |-> v, s |-> s, t |-> n])
     /\ hist' = IF held[n][k].v # 0
                THEN [hist EXCEPT ![n] = @ \cup {Rec(k, held[n][k])}] ELSE hist
     /\ pub' = [pub EXCEPT ![n] = {p}]
     /\ net' = net \cup SendAll(n, p)
     /\ taught' = taught \cup {p}
     /\ nTeach' = nTeach + 1
     /\ p5bad' = (p5bad \/ (seen[n][k].s > 0 /\ ~(s > seen[n][k].s)))
     /\ UNCHANGED <<last, push, nDrop, nEvict, nRestart>>

Wins(n, m, h) ==
  CASE Mut = "M2" -> TRUE                                   \* 1-3a: last arrival
    [] Mut = "M3" /\ n = BadNode -> m.s > h.s \/ (m.s = h.s /\ m.o > h.t)
    [] OTHER -> Beats(m.s, m.o, h.s, h.t)

(* mind_net_task's receive 5116-5240 *)
Recv(m) ==
  /\ m \in net
  /\ LET n == m.to
         k == m.k
         h == held[n][k]
         c == IF Mut = "M1" THEN clk[n] ELSE Max(clk[n], m.s)   \* 5152
         rest == net \ {m} IN
     IF m.o = n \/ m.s = last[n][m.o]          \* own echo 5116, once-per-seq 5137
     THEN /\ net' = rest
          /\ UNCHANGED <<held, clk, last, hist, pub, push, taught, nTeach,
                         nDrop, nEvict, nRestart, seen, p4bad, p5bad>>
     ELSE
       /\ last' = [last EXCEPT ![n][m.o] = m.s]
       /\ UNCHANGED <<pub, taught, nTeach, nDrop, nEvict, nRestart, p5bad>>
       /\ CASE h.v = 0 ->                         \* learn 5224, stamp 5233
                 /\ clk' = [clk EXCEPT ![n] = c + 1]
                 /\ SetHeld(n, k, [v |-> m.v, s |-> m.s, t |-> m.o])
                 /\ UNCHANGED <<hist, push>>
                 /\ net' = rest
            [] h.v = m.v /\ DupAdopt /\ Wins(n, m, h) ->
                 \* candidate fix: the same value with a better stamp ->
                 \* take its stamp and teacher (the value does not change)
                 /\ clk' = [clk EXCEPT ![n] = c]
                 /\ SetHeld(n, k, [v |-> h.v, s |-> m.s, t |-> m.o])
                 /\ UNCHANGED <<hist, push>>
                 /\ net' = rest
            [] h.v = m.v ->                       \* "duplicate ... dropped" 5159
                 /\ clk' = [clk EXCEPT ![n] = c]
                 /\ UNCHANGED <<held, hist, push, seen, p4bad>>
                 /\ net' = rest
            [] Wins(n, m, h) ->                   \* mc_note + revise 5182-5191
                 /\ clk' = [clk EXCEPT ![n] = c + 1]
                 /\ SetHeld(n, k, [v |-> m.v, s |-> m.s, t |-> m.o])
                 /\ hist' = [hist EXCEPT ![n] = @ \cup {Rec(k, h)}]
                 /\ UNCHANGED push
                 /\ net' = rest
            [] OTHER ->                           \* mc_incoming_lost 3497
                 /\ clk' = [clk EXCEPT ![n] = c]
                 /\ hist' = [hist EXCEPT ![n] = @ \cup
                               {[k |-> k, v |-> m.v, s |-> m.s, t |-> m.o]}]
                 /\ UNCHANGED <<held, seen, p4bad>>
                 /\ IF Mut = "M4"
                    THEN UNCHANGED push /\ net' = rest
                    ELSE LET pp == Pkt(h.t, k, h.s, h.v) IN
                         /\ push' = [push EXCEPT ![n] = {pp}]
                         /\ net' = rest \cup SendAll(n, pp)

(* m_republish_last 3087: the last own teach, again, for ever *)
Republish(n) ==
  /\ pub[n] # {}
  /\ LET p == CHOOSE x \in pub[n] : TRUE IN
     /\ ~(SendAll(n, p) \subseteq net)
     /\ net' = net \cup SendAll(n, p)
  /\ UNCHANGED <<held, clk, last, hist, pub, push, taught, nTeach, nDrop,
                 nEvict, nRestart, seen, p4bad, p5bad>>

(* mc_push_redrive 3543: only while the winner is still held (the C caps it
   at 20 sends; here unbounded) *)
PushRedrive(n) ==
  /\ push[n] # {}
  /\ LET pp == CHOOSE x \in push[n] : TRUE IN
     /\ held[n][pp.k] = [v |-> pp.v, s |-> pp.s, t |-> pp.o]
     /\ ~(SendAll(n, pp) \subseteq net)
     /\ net' = net \cup SendAll(n, pp)
  /\ UNCHANGED <<held, clk, last, hist, pub, push, taught, nTeach, nDrop,
                 nEvict, nRestart, seen, p4bad, p5bad>>

(* mq_answer_build 4674: to an asker that does not hold k; the held stamp
   (4716); a teacher equal to the asker is replaced by me (4705) *)
Answer(n, a, k) ==
  /\ WithAnswer
  /\ n # a
  /\ held[a][k].v = 0
  /\ held[n][k].v # 0
  /\ LET h == held[n][k]
         p == Pkt(IF h.t = a THEN n ELSE h.t, k, h.s, h.v) IN
     /\ ~(SendAll(n, p) \subseteq net)
     /\ net' = net \cup SendAll(n, p)
  /\ UNCHANGED <<held, clk, last, hist, pub, push, taught, nTeach, nDrop,
                 nEvict, nRestart, seen, p4bad, p5bad>>

Drop(m) ==
  /\ nDrop < MaxDrop /\ m \in net
  /\ net' = net \ {m} /\ nDrop' = nDrop + 1
  /\ UNCHANGED <<held, clk, last, hist, pub, push, taught, nTeach, nEvict,
                 nRestart, seen, p4bad, p5bad>>

(* r3_fact_learn's forgetting 1963: the engram goes, the rest stays *)
Evict(n, k) ==
  /\ nEvict < MaxEvict /\ held[n][k].v # 0
  /\ held' = [held EXCEPT ![n][k] = None] /\ nEvict' = nEvict + 1
  /\ UNCHANGED <<clk, last, hist, pub, push, net, taught, nTeach, nDrop,
                 nRestart, seen, p4bad, p5bad>>

(* a restart: the clock and every RAM table back to zero (ghosts stay) *)
Restart(n) ==
  /\ nRestart < MaxRestart
  /\ held' = [held EXCEPT ![n] = [k \in Keys |-> None]]
  /\ clk' = [clk EXCEPT ![n] = 0]
  /\ last' = [last EXCEPT ![n] = [o \in Nodes |-> 0]]
  /\ hist' = [hist EXCEPT ![n] = {}]
  /\ pub' = [pub EXCEPT ![n] = {}]
  /\ push' = [push EXCEPT ![n] = {}]
  /\ nRestart' = nRestart + 1
  /\ UNCHANGED <<net, taught, nTeach, nDrop, nEvict, seen, p4bad, p5bad>>

Next ==
  \/ \E n \in Nodes, k \in Keys, v \in Vals : Teach(n, k, v)
  \/ \E m \in net : Recv(m) \/ Drop(m)
  \/ \E n \in Nodes : Republish(n) \/ PushRedrive(n) \/ Restart(n)
  \/ \E n, a \in Nodes, k \in Keys : Answer(n, a, k)
  \/ \E n \in Nodes, k \in Keys : Evict(n, k)

Spec == Init /\ [][Next]_vars

\* L1: every packet that can exist is received if it stays offered;
\* own re-sends and push-backs keep being offered.
Msgs == [to : Nodes, o : Nodes, k : Keys, s : 0..StampMax, v : Vals]
Fair == /\ \A m \in Msgs : WF_vars(Recv(m))
        /\ \A n \in Nodes : WF_vars(Republish(n)) /\ WF_vars(PushRedrive(n))
FairSpec == Spec /\ Fair
\* the same plus weak fairness on Answer (a reading, not the design's line):
\* the C asks again for a key it never held (mh_tick 4491 advertises held keys
\* every 4 ticks, r3_want_hint -> want -> pull), so a pull that stays possible
\* is answered. Only for runs without Evict/Restart: the C does not ask again
\* for a key it once held, while Answer here serves any node that holds none.
FairA == Fair /\ \A n, a \in Nodes, k \in Keys : WF_vars(Answer(n, a, k))
FairASpec == Spec /\ FairA

----------------------------------------------------------------------------
TaughtKeys == { p.k : p \in taught }
\* the teach the rule should leave everywhere: largest stamp, then smaller node
Top(k) == CHOOSE p \in taught : p.k = k /\
            \A q \in taught : q.k = k => (q = p \/ ~Beats(q.s, q.o, p.s, p.o))
Quiet == nDrop = 0 /\ net = {}

TypeOK == /\ net \subseteq [to : Nodes, o : Nodes, k : Keys, s : Nat, v : Vals]
          /\ nTeach <= MaxTeach

\* P1 same answer: no drops and nothing in flight -> every holder agrees
P1 == Quiet => \A k \in Keys : \A a, b \in Nodes :
        (held[a][k].v # 0 /\ held[b][k].v # 0) => held[a][k].v = held[b][k].v
\* P2 order-independent: that answer is the value of Top(k)
P2 == Quiet => \A k \in TaughtKeys : \A n \in Nodes :
        held[n][k].v # 0 => held[n][k].v = Top(k).v
\* P3 nothing learnt is lost: a teach whose value is not the answer is in a hist
P3 == Quiet => \A p \in taught :
        p.v # Top(p.k).v =>
          \E n \in Nodes : [k |-> p.k, v |-> p.v, s |-> p.s, t |-> p.o] \in hist[n]
\* P3v the same at the value level: the losing value is in some hist (the
\* exact stamp and teacher may be another teach of the same value)
P3v == Quiet => \A p \in taught :
        p.v # Top(p.k).v =>
          \E n \in Nodes : \E r \in hist[n] : r.k = p.k /\ r.v = p.v
\* P4 no coming back: what a node holds never ranks below what it once held
P4 == ~p4bad
\* P5 a correction made after seeing v beats v
P5 == ~p5bad

Agreed == \A k \in TaughtKeys : \A n \in Nodes : held[n][k].v = Top(k).v
L1 == <>[]Agreed
=============================================================================
