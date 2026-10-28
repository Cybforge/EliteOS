#ifndef ZENITH_PARALLEL_H
#define ZENITH_PARALLEL_H

/* Run fn(0..n-1) spread over all CPU cores; returns when all are done. */
void parallel_for(int n, void (*fn)(int i, void *ctx), void *ctx);
int  parallel_workers(void);

#endif
