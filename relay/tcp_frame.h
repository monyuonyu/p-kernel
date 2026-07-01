/*
 *  relay/tcp_frame.h — connect-anywhere SLICE 3: TCP length-framing.
 *
 *  THE LOAD-BEARING DETAIL (docs/architecture/20-architecture/connect-anywhere.md §3):
 *  TCP is a byte stream with no datagram boundaries, so a naive socat
 *  UDP<->TCP tunnel concatenates frames and the receiver cannot re-split
 *  them. The fix is an explicit length prefix per packet on the stream:
 *
 *      [u16 big-endian length][the v2 packet bytes]
 *
 *  The v2 packet body + HMAC framing (magic/ver/type/src/dst/nonce/HMAC) is
 *  reused VERBATIM from net_relay.c / relay.c — the ONLY addition is the
 *  2-byte length prefix. The receiver reads the 2-byte length, then exactly
 *  `length` bytes, reassembling ACROSS short reads (the part socat cannot do).
 *
 *  This header is the SINGLE SOURCE OF TRUTH for the framing, shared (drift-
 *  free) by BOTH net_relay_tcp.c twins, the relay's TCP accept loop, and the
 *  de-framer unit cert. It is pure memory logic: NO sockets, NO stdint (so it
 *  coexists with the T-Kernel stdint shadow the kernel TU is built under),
 *  NO allocation, NO task-stack-heavy locals (callers keep the reassembler
 *  static — see feedback_hosted_relay_stack_overflow).
 *
 *  HOSTED-ONLY / crown-safe: never linked into a bare-metal image.
 */
#ifndef PKERNEL_TCP_FRAME_H
#define PKERNEL_TCP_FRAME_H

#define TCP_FRAME_LENPFX 2          /* u16 big-endian length prefix          */
/* Headroom for reassembly: one max relay packet (12+24+1380 = 1416) plus the
 * start of the next, with slack. Caller keeps this struct STATIC, never on a
 * task stack. */
#define TCP_FRAME_MAXBUF 4096

/* Per-connection reassembly buffer. One TcpReasm per accepted TCP stream. */
typedef struct {
    unsigned char buf[TCP_FRAME_MAXBUF];
    int           len;              /* valid bytes currently buffered        */
} TcpReasm;

static inline void tcp_reasm_init(TcpReasm *r) { r->len = 0; }

/* Encode one packet for the stream: prepend the 2-byte big-endian length.
 * `out` must hold pktlen + TCP_FRAME_LENPFX bytes. Returns total bytes
 * written, or -1 if pktlen is out of the u16 range. */
static inline int tcp_frame_encode(unsigned char *out,
                                   const unsigned char *pkt, int pktlen)
{
    if (pktlen < 0 || pktlen > 0xffff) return -1;
    out[0] = (unsigned char)((pktlen >> 8) & 0xff);   /* big-endian high */
    out[1] = (unsigned char)(pktlen & 0xff);          /* big-endian low  */
    for (int i = 0; i < pktlen; i++) out[TCP_FRAME_LENPFX + i] = pkt[i];
    return pktlen + TCP_FRAME_LENPFX;
}

/* Append up to n freshly-read stream bytes to the reassembler. Returns the
 * number of bytes actually appended (fewer than n only if the buffer is
 * full — a stuck/garbage peer; the caller should drop such a connection). */
static inline int tcp_reasm_push(TcpReasm *r, const unsigned char *data, int n)
{
    if (n < 0) n = 0;
    int space = TCP_FRAME_MAXBUF - r->len;
    if (n > space) n = space;
    for (int i = 0; i < n; i++) r->buf[r->len + i] = data[i];
    r->len += n;
    return n;
}

/* Pop exactly ONE complete framed packet into `out` (capacity outcap),
 * advancing the buffer. Returns:
 *    > 0  the packet length (payload copied to out), buffer advanced
 *    = 0  no complete frame yet — need more bytes (short read in progress)
 *    = -1 framed length exceeds outcap (protocol error; caller drops conn)
 * Reassembles across arbitrary chunk splits: state lives entirely in *r, so
 * feeding 1 byte at a time yields the identical frames as feeding all at once.
 */
static inline int tcp_reasm_next(TcpReasm *r, unsigned char *out, int outcap)
{
    if (r->len < TCP_FRAME_LENPFX) return 0;        /* not even a length yet */
    int flen = ((int)r->buf[0] << 8) | (int)r->buf[1];   /* big-endian u16   */
    if (flen > outcap) return -1;                   /* would overflow out    */
    if (r->len < TCP_FRAME_LENPFX + flen) return 0; /* body not all here yet */
    for (int i = 0; i < flen; i++) out[i] = r->buf[TCP_FRAME_LENPFX + i];
    int consumed = TCP_FRAME_LENPFX + flen;
    int rem = r->len - consumed;
    for (int i = 0; i < rem; i++) r->buf[i] = r->buf[consumed + i];
    r->len = rem;
    return flen;
}

#endif /* PKERNEL_TCP_FRAME_H */
