/*
 * Chess: a complete rules engine and an alpha-beta opponent that searches
 * the root moves on every CPU core at once.
 *
 * Board: 10x12 mailbox. Search: iterative deepening negamax with
 * quiescence search, MVV-LVA and killer move ordering, check extension,
 * and root splitting over parallel_for() with a shared alpha bound.
 */
#include <wm.h>
#include <audio.h>
#include <sched.h>
#include <mm.h>
#include <parallel.h>
#include <cpu.h>

enum { EMPTY = 0, PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING };
#define WHITE 0
#define BLACK 8
#define OFFB 0x80
#define COLOR(p) ((p) & 8)
#define TYPE(p) ((p) & 7)

#define F_CAPTURE 1
#define F_EP      2
#define F_CASTLE  4
#define F_DOUBLE  8

#define MOVE(from, to, promo, flags) ((uint32_t)(from) | ((uint32_t)(to) << 7) | ((uint32_t)(promo) << 14) | ((uint32_t)(flags) << 17))
#define M_FROM(m) ((int)((m) & 127))
#define M_TO(m) ((int)(((m) >> 7) & 127))
#define M_PROMO(m) ((int)(((m) >> 14) & 7))
#define M_FLAGS(m) ((int)(((m) >> 17) & 15))

#define INF 1000000
#define MATE 100000
#define MAX_PLY 64
#define MAX_MOVES 256

struct pos {
    uint8_t b[120];
    int side;
    int castle;          /* 1 white short, 2 white long, 4 black short, 8 black long */
    int ep;
    int ksq[2];          /* [0] white king, [1] black king */
    int half;
};

static const int knight_d[8] = { -21, -19, -12, -8, 8, 12, 19, 21 };
static const int king_d[8] = { -11, -10, -9, -1, 1, 9, 10, 11 };
static const int bishop_d[4] = { -11, -9, 9, 11 };
static const int rook_d[4] = { -10, -1, 1, 10 };
static const int value[7] = { 0, 100, 320, 330, 500, 900, 0 };

static int sq(int file, int rank) { return 21 + file + rank * 10; }
static int file_of(int s) { return s % 10 - 1; }
static int rank_of(int s) { return s / 10 - 2; }

static void setup(struct pos *p)
{
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < 120; i++) p->b[i] = OFFB;
    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++) p->b[sq(f, r)] = EMPTY;
    static const int back[8] = { ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK };
    for (int f = 0; f < 8; f++) {
        p->b[sq(f, 0)] = (uint8_t)(back[f] | WHITE);
        p->b[sq(f, 1)] = PAWN | WHITE;
        p->b[sq(f, 6)] = PAWN | BLACK;
        p->b[sq(f, 7)] = (uint8_t)(back[f] | BLACK);
    }
    p->side = WHITE;
    p->castle = 15;
    p->ksq[0] = sq(4, 0);
    p->ksq[1] = sq(4, 7);
}

static bool attacked(const struct pos *p, int s, int by)
{
    /* pawns */
    if (by == WHITE) {
        if (p->b[s - 9] == (PAWN | WHITE) || p->b[s - 11] == (PAWN | WHITE)) return true;
    } else {
        if (p->b[s + 9] == (PAWN | BLACK) || p->b[s + 11] == (PAWN | BLACK)) return true;
    }
    for (int i = 0; i < 8; i++) {
        if (p->b[s + knight_d[i]] == (KNIGHT | by)) return true;
        if (p->b[s + king_d[i]] == (KING | by)) return true;
    }
    for (int i = 0; i < 4; i++) {
        for (int t = s + bishop_d[i];; t += bishop_d[i]) {
            int pc = p->b[t];
            if (pc == EMPTY) continue;
            if (pc != OFFB && COLOR(pc) == by && (TYPE(pc) == BISHOP || TYPE(pc) == QUEEN)) return true;
            break;
        }
        for (int t = s + rook_d[i];; t += rook_d[i]) {
            int pc = p->b[t];
            if (pc == EMPTY) continue;
            if (pc != OFFB && COLOR(pc) == by && (TYPE(pc) == ROOK || TYPE(pc) == QUEEN)) return true;
            break;
        }
    }
    return false;
}

static bool in_check(const struct pos *p, int side)
{
    return attacked(p, p->ksq[side ? 1 : 0], side ^ 8);
}

static int gen(const struct pos *p, uint32_t *mv, bool captures_only)
{
    int n = 0, us = p->side, them = us ^ 8;
    int fwd = us == WHITE ? 10 : -10;
    int start_rank = us == WHITE ? 1 : 6, promo_rank = us == WHITE ? 7 : 0;
    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++) {
            int s = sq(f, r);
            int pc = p->b[s];
            if (pc == EMPTY || COLOR(pc) != us) continue;
            int t = TYPE(pc);
            if (t == PAWN) {
                int to = s + fwd;
                if (p->b[to] == EMPTY) {
                    if (rank_of(to) == promo_rank) {
                        mv[n++] = MOVE(s, to, QUEEN, 0);
                        if (!captures_only) {
                            mv[n++] = MOVE(s, to, KNIGHT, 0);
                            mv[n++] = MOVE(s, to, ROOK, 0);
                            mv[n++] = MOVE(s, to, BISHOP, 0);
                        }
                    } else if (!captures_only) {
                        mv[n++] = MOVE(s, to, 0, 0);
                        if (rank_of(s) == start_rank && p->b[to + fwd] == EMPTY) mv[n++] = MOVE(s, to + fwd, 0, F_DOUBLE);
                    }
                }
                for (int d = -1; d <= 1; d += 2) {
                    int c = s + fwd + d;
                    int cp = p->b[c];
                    if (cp != OFFB && cp != EMPTY && COLOR(cp) == them) {
                        if (rank_of(c) == promo_rank) {
                            mv[n++] = MOVE(s, c, QUEEN, F_CAPTURE);
                            if (!captures_only) {
                                mv[n++] = MOVE(s, c, KNIGHT, F_CAPTURE);
                                mv[n++] = MOVE(s, c, ROOK, F_CAPTURE);
                                mv[n++] = MOVE(s, c, BISHOP, F_CAPTURE);
                            }
                        } else {
                            mv[n++] = MOVE(s, c, 0, F_CAPTURE);
                        }
                    } else if (c == p->ep && p->ep) {
                        mv[n++] = MOVE(s, c, 0, F_CAPTURE | F_EP);
                    }
                }
                continue;
            }
            const int *dirs;
            int nd;
            bool slide;
            switch (t) {
            case KNIGHT: dirs = knight_d; nd = 8; slide = false; break;
            case BISHOP: dirs = bishop_d; nd = 4; slide = true; break;
            case ROOK: dirs = rook_d; nd = 4; slide = true; break;
            case QUEEN: dirs = king_d; nd = 8; slide = true; break;
            default: dirs = king_d; nd = 8; slide = false; break;
            }
            for (int i = 0; i < nd; i++) {
                for (int to = s + dirs[i];; to += dirs[i]) {
                    int tp = p->b[to];
                    if (tp == OFFB) break;
                    if (tp == EMPTY) {
                        if (!captures_only) mv[n++] = MOVE(s, to, 0, 0);
                    } else {
                        if (COLOR(tp) == them) mv[n++] = MOVE(s, to, 0, F_CAPTURE);
                        break;
                    }
                    if (!slide) break;
                }
            }
            if (t == KING && !captures_only) {
                int home = us == WHITE ? sq(4, 0) : sq(4, 7);
                int ks = us == WHITE ? 1 : 4, qs = us == WHITE ? 2 : 8;
                if (s == home && !attacked(p, s, them)) {
                    if ((p->castle & ks) && p->b[s + 1] == EMPTY && p->b[s + 2] == EMPTY &&
                        p->b[s + 3] == (ROOK | us) && !attacked(p, s + 1, them) && !attacked(p, s + 2, them))
                        mv[n++] = MOVE(s, s + 2, 0, F_CASTLE);
                    if ((p->castle & qs) && p->b[s - 1] == EMPTY && p->b[s - 2] == EMPTY && p->b[s - 3] == EMPTY &&
                        p->b[s - 4] == (ROOK | us) && !attacked(p, s - 1, them) && !attacked(p, s - 2, them))
                        mv[n++] = MOVE(s, s - 2, 0, F_CASTLE);
                }
            }
        }
    return n;
}

/* copy-make; returns false (and leaves *out undefined) if the move leaves the king in check */
static bool make(const struct pos *p, uint32_t m, struct pos *o)
{
    *o = *p;
    int from = M_FROM(m), to = M_TO(m), fl = M_FLAGS(m), us = p->side;
    int pc = o->b[from];
    o->b[to] = (uint8_t)pc;
    o->b[from] = EMPTY;
    o->ep = 0;
    o->half = (TYPE(pc) == PAWN || (fl & F_CAPTURE)) ? 0 : p->half + 1;
    if (fl & F_EP) o->b[to + (us == WHITE ? -10 : 10)] = EMPTY;
    if (fl & F_DOUBLE) o->ep = from + (us == WHITE ? 10 : -10);
    if (M_PROMO(m)) o->b[to] = (uint8_t)(M_PROMO(m) | us);
    if (fl & F_CASTLE) {
        if (to > from) { o->b[from + 1] = o->b[from + 3]; o->b[from + 3] = EMPTY; }
        else { o->b[from - 1] = o->b[from - 4]; o->b[from - 4] = EMPTY; }
    }
    if (TYPE(pc) == KING) {
        o->ksq[us ? 1 : 0] = to;
        o->castle &= us == WHITE ? ~3 : ~12;
    }
    /* rooks leaving or being captured on their corners */
    if (from == sq(0, 0) || to == sq(0, 0)) o->castle &= ~2;
    if (from == sq(7, 0) || to == sq(7, 0)) o->castle &= ~1;
    if (from == sq(0, 7) || to == sq(0, 7)) o->castle &= ~8;
    if (from == sq(7, 7) || to == sq(7, 7)) o->castle &= ~4;
    o->side = us ^ 8;
    return !in_check(o, us);
}

static int legal_moves(const struct pos *p, uint32_t *out)
{
    uint32_t mv[MAX_MOVES];
    int n = gen(p, mv, false), k = 0;
    struct pos t;
    for (int i = 0; i < n; i++)
        if (make(p, mv[i], &t)) out[k++] = mv[i];
    return k;
}

/* ------------------------------------------------------------------------
 * evaluation: material plus piece-square tables (a8 = index 0, white's view)
 * ---------------------------------------------------------------------- */

static const int8_t pst[7][64] = {
    { 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 50, 50, 50, 50, 50, 50, 50, 50, 10, 10, 20, 30, 30, 20, 10, 10, 5, 5, 10, 25, 25, 10, 5, 5,
      0, 0, 0, 20, 20, 0, 0, 0, 5, -5, -10, 0, 0, -10, -5, 5, 5, 10, 10, -20, -20, 10, 10, 5, 0, 0, 0, 0, 0, 0, 0, 0 },
    { -50, -40, -30, -30, -30, -30, -40, -50, -40, -20, 0, 0, 0, 0, -20, -40, -30, 0, 10, 15, 15, 10, 0, -30,
      -30, 5, 15, 20, 20, 15, 5, -30, -30, 0, 15, 20, 20, 15, 0, -30, -30, 5, 10, 15, 15, 10, 5, -30,
      -40, -20, 0, 5, 5, 0, -20, -40, -50, -40, -30, -30, -30, -30, -40, -50 },
    { -20, -10, -10, -10, -10, -10, -10, -20, -10, 0, 0, 0, 0, 0, 0, -10, -10, 0, 5, 10, 10, 5, 0, -10,
      -10, 5, 5, 10, 10, 5, 5, -10, -10, 0, 10, 10, 10, 10, 0, -10, -10, 10, 10, 10, 10, 10, 10, -10,
      -10, 5, 0, 0, 0, 0, 5, -10, -20, -10, -10, -10, -10, -10, -10, -20 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 5, 10, 10, 10, 10, 10, 10, 5, -5, 0, 0, 0, 0, 0, 0, -5, -5, 0, 0, 0, 0, 0, 0, -5,
      -5, 0, 0, 0, 0, 0, 0, -5, -5, 0, 0, 0, 0, 0, 0, -5, -5, 0, 0, 0, 0, 0, 0, -5, 0, 0, 0, 5, 5, 0, 0, 0 },
    { -20, -10, -10, -5, -5, -10, -10, -20, -10, 0, 0, 0, 0, 0, 0, -10, -10, 0, 5, 5, 5, 5, 0, -10,
      -5, 0, 5, 5, 5, 5, 0, -5, 0, 0, 5, 5, 5, 5, 0, -5, -10, 5, 5, 5, 5, 5, 0, -10,
      -10, 0, 5, 0, 0, 0, 0, -10, -20, -10, -10, -5, -5, -10, -10, -20 },
    { -30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30,
      -30, -40, -40, -50, -50, -40, -40, -30, -20, -30, -30, -40, -40, -30, -30, -20, -10, -20, -20, -20, -20, -20, -20, -10,
      20, 20, 0, 0, 0, 0, 20, 20, 20, 30, 10, 0, 0, 10, 30, 20 },
};

static const int8_t king_end[64] = {
    -50, -40, -30, -20, -20, -30, -40, -50, -30, -20, -10, 0, 0, -10, -20, -30, -30, -10, 20, 30, 30, 20, -10, -30,
    -30, -10, 30, 40, 40, 30, -10, -30, -30, -10, 30, 40, 40, 30, -10, -30, -30, -10, 20, 30, 30, 20, -10, -30,
    -30, -30, 0, 0, 0, 0, -30, -30, -50, -30, -30, -30, -30, -30, -30, -50,
};

static int evaluate(const struct pos *p)
{
    int score = 0, material = 0;
    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++) {
            int pc = p->b[sq(f, r)];
            if (pc == EMPTY) continue;
            int t = TYPE(pc);
            if (t != KING && t != PAWN) material += value[t];
        }
    bool endgame = material <= 2600;
    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++) {
            int pc = p->b[sq(f, r)];
            if (pc == EMPTY) continue;
            int t = TYPE(pc);
            int idx = COLOR(pc) == WHITE ? (7 - r) * 8 + f : r * 8 + f;
            int v = value[t] + (t == KING && endgame ? king_end[idx] : pst[t][idx]);
            score += COLOR(pc) == WHITE ? v : -v;
        }
    return p->side == WHITE ? score : -score;
}

/* ------------------------------------------------------------------------
 * search
 * ---------------------------------------------------------------------- */

struct search {
    uint32_t moves[MAX_PLY + 16][MAX_MOVES];
    int scores[MAX_PLY + 16][MAX_MOVES];
    uint32_t killer[MAX_PLY + 16][2];
    uint64_t nodes;
};

static volatile bool abort_search;
static uint64_t deadline;
static volatile uint64_t total_nodes;

static void order(struct search *s, int ply, int n, uint32_t *mv, const struct pos *p, uint32_t first)
{
    int *sc = s->scores[ply];
    for (int i = 0; i < n; i++) {
        uint32_t m = mv[i];
        int v = 0;
        if (m == first) v = 1000000;
        else if (M_FLAGS(m) & F_CAPTURE) {
            int victim = (M_FLAGS(m) & F_EP) ? PAWN : TYPE(p->b[M_TO(m)]);
            v = 100000 + value[victim] * 10 - value[TYPE(p->b[M_FROM(m)])] / 10;
        } else if (M_PROMO(m)) v = 90000 + value[M_PROMO(m)];
        else if (m == s->killer[ply][0]) v = 80000;
        else if (m == s->killer[ply][1]) v = 79000;
        sc[i] = v;
    }
}

static uint32_t pick(struct search *s, int ply, int n, uint32_t *mv, int i)
{
    int *sc = s->scores[ply];
    int best = i;
    for (int j = i + 1; j < n; j++) if (sc[j] > sc[best]) best = j;
    uint32_t m = mv[best]; mv[best] = mv[i]; mv[i] = m;
    int t = sc[best]; sc[best] = sc[i]; sc[i] = t;
    return mv[i];
}

static void tick(struct search *s)
{
    if ((++s->nodes & 1023) == 0) {
        __atomic_fetch_add(&total_nodes, 1024, __ATOMIC_RELAXED);
        if (uptime_ms() > deadline) abort_search = true;
    }
}

static int quiesce(struct search *s, const struct pos *p, int alpha, int beta, int ply)
{
    tick(s);
    int stand = evaluate(p);
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;
    if (ply >= MAX_PLY + 12) return alpha;
    uint32_t *mv = s->moves[ply];
    int n = gen(p, mv, true);
    order(s, ply, n, mv, p, 0);
    struct pos c;
    for (int i = 0; i < n; i++) {
        uint32_t m = pick(s, ply, n, mv, i);
        if (!make(p, m, &c)) continue;
        int v = -quiesce(s, &c, -beta, -alpha, ply + 1);
        if (abort_search) return 0;
        if (v >= beta) return beta;
        if (v > alpha) alpha = v;
    }
    return alpha;
}

static int negamax(struct search *s, const struct pos *p, int depth, int alpha, int beta, int ply)
{
    bool check = in_check(p, p->side);
    if (check && ply < MAX_PLY - 2) depth++;
    if (depth <= 0) return quiesce(s, p, alpha, beta, ply);
    tick(s);
    if (p->half >= 100) return 0;
    uint32_t *mv = s->moves[ply];
    int n = gen(p, mv, false);
    order(s, ply, n, mv, p, 0);
    int legal = 0;
    struct pos c;
    for (int i = 0; i < n; i++) {
        uint32_t m = pick(s, ply, n, mv, i);
        if (!make(p, m, &c)) continue;
        legal++;
        int v = -negamax(s, &c, depth - 1, -beta, -alpha, ply + 1);
        if (abort_search) return 0;
        if (v >= beta) {
            if (!(M_FLAGS(m) & F_CAPTURE) && s->killer[ply][0] != m) {
                s->killer[ply][1] = s->killer[ply][0];
                s->killer[ply][0] = m;
            }
            return beta;
        }
        if (v > alpha) alpha = v;
    }
    if (!legal) return check ? -MATE + ply : 0;
    return alpha;
}

/* root: search the moves in parallel, sharing the best score found so far */
#define POOL 65
static struct search *pool[POOL];
static volatile int pool_busy[POOL];
static int pool_n;

/* each thread borrows a search context (move stacks, killer moves) */
static struct search *ctx_get(void)
{
    for (;;)
        for (int i = 0; i < pool_n; i++) {
            int z = 0;
            if (__atomic_compare_exchange_n(&pool_busy[i], &z, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return pool[i];
        }
}

static void ctx_put(struct search *s)
{
    for (int i = 0; i < pool_n; i++)
        if (pool[i] == s) __atomic_store_n(&pool_busy[i], 0, __ATOMIC_RELEASE);
}

struct root_job {
    const struct pos *p;
    uint32_t *moves;
    int *scores;
    int n, depth;
    volatile int alpha;
};

static void root_one(int i, void *arg)
{
    struct root_job *j = arg;
    struct search *s = ctx_get();
    struct pos c;
    make(j->p, j->moves[i], &c);
    int a = __atomic_load_n(&j->alpha, __ATOMIC_RELAXED);
    int v = -negamax(s, &c, j->depth - 1, -INF, -a, 1);
    ctx_put(s);
    if (abort_search) { j->scores[i] = -INF; return; }
    j->scores[i] = v;
    int cur = __atomic_load_n(&j->alpha, __ATOMIC_RELAXED);
    while (v > cur && !__atomic_compare_exchange_n(&j->alpha, &cur, v, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

struct think_result { uint32_t move; int score, depth; uint64_t nodes, ms; };

static struct think_result think(const struct pos *p, int max_depth, int ms_budget)
{
    struct think_result res = { 0 };
    uint32_t moves[MAX_MOVES];
    int scores[MAX_MOVES];
    int n = legal_moves(p, moves);
    if (!n) return res;
    res.move = moves[0];
    if (n == 1) return res;
    if (!pool_n) {
        pool_n = MIN(POOL, parallel_workers() + 1);
        for (int i = 0; i < pool_n; i++) pool[i] = kzalloc(sizeof(struct search));
    }
    for (int i = 0; i < pool_n; i++) pool[i]->nodes = 0;
    abort_search = false;
    total_nodes = 0;
    uint64_t t0 = uptime_ms();
    deadline = t0 + (uint64_t)ms_budget;
    for (int d = 1; d <= max_depth; d++) {
        struct root_job j = { p, moves, scores, n, d, -INF };
        /* the best move of the last iteration goes first and is searched alone,
           so everything else starts with a good bound */
        root_one(0, &j);
        if (abort_search) break;
        if (n > 1) {
            struct root_job j2 = j;
            j2.moves = moves + 1;
            j2.scores = scores + 1;
            j2.alpha = j.alpha;
            parallel_for(n - 1, root_one, &j2);
            if (abort_search) break;
        }
        /* sort by score, best first (stable insertion sort) */
        for (int i = 1; i < n; i++)
            for (int k = i; k > 0 && scores[k] > scores[k - 1]; k--) {
                int ts = scores[k]; scores[k] = scores[k - 1]; scores[k - 1] = ts;
                uint32_t tm = moves[k]; moves[k] = moves[k - 1]; moves[k - 1] = tm;
            }
        res.move = moves[0];
        res.score = scores[0];
        res.depth = d;
        if (scores[0] > MATE - 100 || uptime_ms() - t0 > (uint64_t)ms_budget / 3) break;
    }
    res.ms = uptime_ms() - t0;
    res.nodes = total_nodes;
    for (int i = 0; i < pool_n; i++) res.nodes += pool[i]->nodes & 1023;
    return res;
}

/* ------------------------------------------------------------------------
 * the game
 * ---------------------------------------------------------------------- */

#define HISTORY 512

struct chess {
    app_t *app;
    struct pos pos;
    struct pos hist[HISTORY];
    uint32_t moves[HISTORY];
    int nhist;
    int human;                 /* WHITE or BLACK */
    int level;                 /* 0 easy, 1 medium, 2 hard */
    int sel;                   /* selected square or -1 */
    uint32_t targets[MAX_MOVES];
    int ntargets;
    uint32_t last;
    volatile bool thinking;
    struct think_result ai;
    uint32_t anim_move;
    int anim_piece;
    uint64_t anim_t0;
    bool flipped;
    bool closed;
    char status[96];
};

static const char *piece_glyph(int t, bool filled)
{
    static const char *filled_g[7] = { "", "♟", "♞", "♝", "♜", "♛", "♚" };
    static const char *outline_g[7] = { "", "♙", "♘", "♗", "♖", "♕", "♔" };
    return filled ? filled_g[t] : outline_g[t];
}

static void move_name(const struct pos *p, uint32_t m, char *out, size_t n)
{
    static const char letters[7] = { 0, 0, 'N', 'B', 'R', 'Q', 'K' };
    int from = M_FROM(m), to = M_TO(m);
    int t = TYPE(p->b[from]);
    if (M_FLAGS(m) & F_CASTLE) { strlcpy(out, to > from ? "O-O" : "O-O-O", n); return; }
    size_t o = 0;
    if (letters[t]) out[o++] = letters[t];
    else if (M_FLAGS(m) & F_CAPTURE) out[o++] = (char)('a' + file_of(from));
    if (M_FLAGS(m) & F_CAPTURE) out[o++] = 'x';
    out[o++] = (char)('a' + file_of(to));
    out[o++] = (char)('1' + rank_of(to));
    if (M_PROMO(m)) { out[o++] = '='; out[o++] = letters[M_PROMO(m)]; }
    struct pos c;
    make(p, m, &c);
    uint32_t tmp[MAX_MOVES];
    if (in_check(&c, c.side)) out[o++] = legal_moves(&c, tmp) ? '+' : '#';
    out[o] = 0;
}

static void update_status(struct chess *g)
{
    uint32_t tmp[MAX_MOVES];
    int n = legal_moves(&g->pos, tmp);
    bool check = in_check(&g->pos, g->pos.side);
    const char *who = g->pos.side == WHITE ? "White" : "Black";
    if (!n && check) snprintf(g->status, sizeof(g->status), "Checkmate - %s wins", g->pos.side == WHITE ? "Black" : "White");
    else if (!n) strlcpy(g->status, "Stalemate - draw", sizeof(g->status));
    else if (g->pos.half >= 100) strlcpy(g->status, "Draw by the fifty-move rule", sizeof(g->status));
    else snprintf(g->status, sizeof(g->status), "%s to move%s", who, check ? " - check!" : "");
}

static bool game_over(struct chess *g)
{
    uint32_t tmp[MAX_MOVES];
    return !legal_moves(&g->pos, tmp) || g->pos.half >= 100;
}

static void play(struct chess *g, uint32_t m)
{
    if (g->nhist >= HISTORY) return;
    bool capture = M_FLAGS(m) & F_CAPTURE;
    g->hist[g->nhist] = g->pos;
    g->moves[g->nhist] = m;
    g->nhist++;
    g->anim_move = m;
    g->anim_piece = g->pos.b[M_FROM(m)];
    g->anim_t0 = uptime_ms();
    struct pos c;
    make(&g->pos, m, &c);
    g->pos = c;
    g->last = m;
    g->sel = -1;
    g->ntargets = 0;
    update_status(g);
    if (game_over(g)) audio_sound(in_check(&g->pos, g->pos.side) ? SND_SUCCESS : SND_NOTIFY);
    else if (in_check(&g->pos, g->pos.side)) audio_sound(SND_NOTIFY);
    else audio_sound(capture ? SND_POP : SND_CLICK);
}

static void kick(struct chess *g)
{
    if (g->closed) return;
    struct gui_event ev = { 0 };
    ev.type = EV_TIMER;
    wm_post_event(g->app->win, &ev);
}

static int ai_thread(void *arg)
{
    struct chess *g = arg;
    static const int depth[3] = { 2, 4, 32 };
    static const int budget[3] = { 800, 2500, 5000 };
    struct pos p = g->pos;
    struct think_result r = think(&p, depth[g->level], budget[g->level]);
    g->ai = r;
    g->thinking = false;
    kick(g);
    return 0;
}

static void maybe_ai(struct chess *g)
{
    if (g->thinking || g->pos.side == g->human || game_over(g)) return;
    g->thinking = true;
    thread_create_ex("chess-ai", ai_thread, g, 0, -1, NULL, 128 * 1024);
}

static void new_game(struct chess *g, int human)
{
    if (g->thinking) return;
    setup(&g->pos);
    g->nhist = 0;
    g->human = human;
    g->flipped = human == BLACK;
    g->sel = -1;
    g->ntargets = 0;
    g->last = 0;
    g->anim_move = 0;
    memset(&g->ai, 0, sizeof(g->ai));
    update_status(g);
    maybe_ai(g);
}

/* ------------------------------------------------------------------------
 * drawing
 * ---------------------------------------------------------------------- */

static void draw_piece(surface_t *s, int pc, int cx, int cy)
{
    int t = TYPE(pc);
    const char *fill = piece_glyph(t, true), *outline = piece_glyph(t, false);
    int w = font_text_width(font_chess, fill);
    int x = cx - w / 2, y = cy - font_height(font_chess) / 2 + 2;
    bool white = COLOR(pc) == WHITE;
    gfx_text(s, font_chess, x + 1, y + 3, fill, ALPHA(0, 70));                 /* shadow */
    if (white) {
        /* dark rim, ivory body, the outline glyph only as faint inner detail */
        static const int8_t ox[8] = { -1, 1, 0, 0, -1, 1, -1, 1 }, oy[8] = { 0, 0, -1, 1, -1, -1, 1, 1 };
        for (int k = 0; k < 8; k++) gfx_text(s, font_chess, x + ox[k], y + oy[k], fill, HEX(0x2B2733));
        gfx_text(s, font_chess, x, y, fill, HEX(0xFBF7EE));
        gfx_text(s, font_chess, x, y, outline, ALPHA(0x2B2733, 90));
    } else {
        gfx_text(s, font_chess, x, y, fill, HEX(0x23202B));
        gfx_text(s, font_chess, x, y, outline, ALPHA(0x9A94A8, 120));
    }
}

static int square_at(struct chess *g, rect_t board, int mx, int my)
{
    if (!rect_has(board, mx, my)) return -1;
    int cs = board.w / 8;
    int f = (mx - board.x) / cs, r = 7 - (my - board.y) / cs;
    if (g->flipped) { f = 7 - f; r = 7 - r; }
    return sq(f, r);
}

static void square_xy(struct chess *g, rect_t board, int s, int *x, int *y)
{
    int cs = board.w / 8;
    int f = file_of(s), r = rank_of(s);
    if (g->flipped) { f = 7 - f; r = 7 - r; }
    *x = board.x + f * cs;
    *y = board.y + (7 - r) * cs;
}

static void chess_paint(app_t *a, surface_t *s)
{
    struct chess *g = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, theme.bg);

    int cs = MIN((H - 48) / 8, (W - 330) / 8);
    rect_t board = R(24, (H - cs * 8) / 2, cs * 8, cs * 8);
    gfx_shadow(s, board.x, board.y + 6, board.w, board.h, 8, 24, 120);
    color_t light = HEX(0xEEE3CF), dark = HEX(0x8C6FB0);
    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++) {
            int x = board.x + f * cs, y = board.y + r * cs;
            bool lsq = (r + f) % 2 == 0;
            gfx_fill(s, x, y, cs, cs, lsq ? light : dark);
        }
    /* coordinates */
    for (int i = 0; i < 8; i++) {
        char c[2] = { 0, 0 };
        c[0] = (char)('a' + (g->flipped ? 7 - i : i));
        gfx_text(s, font_ui_md, board.x + i * cs + cs - 11, board.y + board.h - 17, c, (i % 2) ? HEX(0x8C6FB0) : HEX(0xEEE3CF));
        c[0] = (char)('8' - (g->flipped ? 7 - i : i));
        gfx_text(s, font_ui_md, board.x + 4, board.y + i * cs + 3, c, (i % 2) ? HEX(0xEEE3CF) : HEX(0x8C6FB0));
    }
    /* last move and selection */
    if (g->last) {
        for (int k = 0; k < 2; k++) {
            int x, y;
            square_xy(g, board, k ? M_TO(g->last) : M_FROM(g->last), &x, &y);
            gfx_fill(s, x, y, cs, cs, ALPHA(0xFFD54F, 90));
        }
    }
    if (g->sel >= 0) {
        int x, y;
        square_xy(g, board, g->sel, &x, &y);
        gfx_fill(s, x, y, cs, cs, ALPHA(0x7C5CFF, 110));
    }
    /* king in check */
    if (in_check(&g->pos, g->pos.side)) {
        int x, y;
        square_xy(g, board, g->pos.ksq[g->pos.side ? 1 : 0], &x, &y);
        gfx_circle(s, (float)x + cs / 2.0f, (float)y + cs / 2.0f, cs * 0.48f, ALPHA(0xFF3B5C, 150));
    }
    /* pieces (the moving one is animated) */
    uint64_t now = uptime_ms();
    float at = g->anim_move ? (float)(now - g->anim_t0) / 180.0f : 1.0f;
    bool animating = at < 1.0f;
    for (int r = 0; r < 8; r++)
        for (int f = 0; f < 8; f++) {
            int sqi = sq(f, r);
            int pc = g->pos.b[sqi];
            if (pc == EMPTY) continue;
            if (animating && sqi == M_TO(g->anim_move)) continue;
            int x, y;
            square_xy(g, board, sqi, &x, &y);
            draw_piece(s, pc, x + cs / 2, y + cs / 2);
        }
    if (animating) {
        int x0, y0, x1, y1;
        square_xy(g, board, M_FROM(g->anim_move), &x0, &y0);
        square_xy(g, board, M_TO(g->anim_move), &x1, &y1);
        float e = 1 - (1 - at) * (1 - at);
        draw_piece(s, g->anim_piece, (int)((float)x0 + (float)(x1 - x0) * e) + cs / 2, (int)((float)y0 + (float)(y1 - y0) * e) + cs / 2);
        u->want_repaint = true;
    }
    /* legal targets */
    for (int i = 0; i < g->ntargets; i++) {
        int x, y;
        square_xy(g, board, M_TO(g->targets[i]), &x, &y);
        if (g->pos.b[M_TO(g->targets[i])] != EMPTY)
            gfx_ring(s, (float)x + cs / 2.0f, (float)y + cs / 2.0f, cs * 0.44f, 4, ALPHA(0x1D1530, 110));
        else
            gfx_circle(s, (float)x + cs / 2.0f, (float)y + cs / 2.0f, cs * 0.15f, ALPHA(0x1D1530, 110));
    }

    /* side panel */
    int px = board.x + board.w + 28, pw = W - px - 20;
    gfx_text(s, font_bold_lg, px, board.y, "Chess", theme.text);
    gfx_text_ellipsis(s, font_ui_md, px, board.y + 34, pw, g->status, game_over(g) ? theme.warning : theme.text_dim);
    int y = board.y + 70;
    if (g->thinking) {
        float ph = (float)(now % 1200) / 1200.0f;
        for (int i = 0; i < 3; i++) {
            float k = (float)k_sin((ph - (float)i * 0.15f) * 6.2831853f) * 0.5f + 0.5f;
            gfx_circle(s, (float)px + 6 + (float)i * 16, (float)y + 8, 4 + 2 * k, ALPHA(0x7C5CFF, (int)(120 + 135 * k)));
        }
        char b[64];
        snprintf(b, sizeof(b), "thinking on %d cores...", parallel_workers());
        gfx_text(s, font_ui, px + 56, y + 1, b, theme.text_dim);
        u->want_repaint = true;
    } else if (g->ai.depth) {
        char b[96];
        uint64_t nps = g->ai.ms ? g->ai.nodes * 1000 / g->ai.ms : 0;
        int sc = g->ai.score;
        if (sc > MATE - 100) snprintf(b, sizeof(b), "depth %d · mate · %lu kn/s", g->ai.depth, nps / 1000);
        else snprintf(b, sizeof(b), "depth %d · eval %+d.%02d · %lu kn/s", g->ai.depth, sc / 100, (sc < 0 ? -sc : sc) % 100,
                      nps / 1000);
        gfx_text_ellipsis(s, font_ui, px, y + 1, pw, b, theme.text_faint);
    }
    y += 34;
    /* move list */
    rect_t ml = R(px, y, pw, board.y + board.h - 150 - y);
    gfx_round_rect(s, ml.x, ml.y, ml.w, ml.h, 10, theme.panel);
    int rows = (ml.h - 16) / 22, total_rows = (g->nhist + 1) / 2;
    int first_row = MAX(0, total_rows - rows);
    for (int r = first_row; r < total_rows; r++) {
        int yy = ml.y + 8 + (r - first_row) * 22;
        char b[16];
        snprintf(b, sizeof(b), "%d.", r + 1);
        gfx_text(s, font_ui, ml.x + 12, yy, b, theme.text_faint);
        for (int k = 0; k < 2; k++) {
            int i = r * 2 + k;
            if (i >= g->nhist) break;
            char mn[16];
            move_name(&g->hist[i], g->moves[i], mn, sizeof(mn));
            gfx_text(s, font_ui_md, ml.x + 50 + k * ((ml.w - 60) / 2), yy, mn, theme.text);
        }
    }
    /* controls */
    int by = board.y + board.h - 140;
    gfx_text(s, font_ui_bold, px, by, "Difficulty", theme.text_faint);
    static const char *lv[3] = { "Easy", "Medium", "Hard" };
    for (int i = 0; i < 3; i++) {
        rect_t r = R(px + i * (pw / 3), by + 22, pw / 3 - 6, 30);
        if (ui_list_item(u, r, g->level == i)) g->level = i;
        gfx_text_center(s, font_ui_md, r, lv[i], g->level == i ? theme.text : theme.text_dim);
    }
    if (ui_button(u, R(px, by + 64, pw / 2 - 4, 34), "New (White)", BTN_PRIMARY)) new_game(g, WHITE);
    if (ui_button(u, R(px + pw / 2 + 4, by + 64, pw / 2 - 4, 34), "New (Black)", BTN_NORMAL)) new_game(g, BLACK);
    if (ui_button(u, R(px, by + 106, pw / 2 - 4, 34), "Undo", BTN_NORMAL) && !g->thinking && g->nhist) {
        /* take back the AI's reply and our move */
        int back = g->nhist >= 2 && g->pos.side == g->human ? 2 : 1;
        g->nhist -= back;
        g->pos = g->hist[g->nhist];
        g->last = g->nhist ? g->moves[g->nhist - 1] : 0;
        g->sel = -1;
        g->ntargets = 0;
        g->anim_move = 0;
        update_status(g);
    }
    if (ui_button(u, R(px + pw / 2 + 4, by + 106, pw / 2 - 4, 34), "Flip board", BTN_NORMAL)) g->flipped = !g->flipped;

    /* input */
    if (u->mpressed && !g->thinking && g->pos.side == g->human && !game_over(g)) {
        int t = square_at(g, board, u->mx, u->my);
        if (t >= 0) {
            bool moved = false;
            for (int i = 0; i < g->ntargets; i++)
                if (M_TO(g->targets[i]) == t && (!M_PROMO(g->targets[i]) || M_PROMO(g->targets[i]) == QUEEN)) {
                    play(g, g->targets[i]);
                    moved = true;
                    break;
                }
            if (!moved) {
                int pc = g->pos.b[t];
                g->ntargets = 0;
                g->sel = -1;
                if (pc != EMPTY && COLOR(pc) == g->human) {
                    uint32_t all[MAX_MOVES];
                    int n = legal_moves(&g->pos, all);
                    for (int i = 0; i < n; i++)
                        if (M_FROM(all[i]) == t) g->targets[g->ntargets++] = all[i];
                    g->sel = t;
                }
            }
            u->want_repaint = true;
        }
    }
    /* the AI's move arrives */
    if (!g->thinking && g->ai.move && g->pos.side != g->human) {
        uint32_t m = g->ai.move;
        g->ai.move = 0;
        play(g, m);
        u->want_repaint = true;
    }
    if (!animating) maybe_ai(g);
}

static int chess_event(app_t *a, struct gui_event *ev)
{
    UNUSED(a);
    return ev->type == EV_TIMER;
}

static void chess_close(app_t *a)
{
    struct chess *g = a->data;
    g->closed = true;
    abort_search = true;
    while (g->thinking) sched_sleep(10);
    a->quit = true;
}

int chess_main(void *arg)
{
    UNUSED(arg);
    struct chess *g = kzalloc(sizeof(*g));
    app_t a = { 0 };
    g->app = &a;
    a.data = g;
    g->level = 1;
    a.win = wm_create("Chess", 900, 620, WF_RESIZABLE);
    if (!a.win) { kfree(g); return 1; }
    wm_set_icon(a.win, ICON_CHESS);
    wm_set_min_size(a.win, 760, 520);
    a.on_paint = chess_paint;
    a.on_event = chess_event;
    a.on_close = chess_close;
    new_game(g, WHITE);
    int r = app_run(&a);
    kfree(g);
    return r;
}
