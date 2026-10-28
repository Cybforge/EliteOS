package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

func main() {
	procs := 4
	iters := 200
	if len(os.Args) > 1 {
		procs, _ = strconv.Atoi(os.Args[1])
	}
	if len(os.Args) > 2 {
		iters, _ = strconv.Atoi(os.Args[2])
	}
	runtime.GOMAXPROCS(procs)
	fmt.Println("gostress: procs", procs, "iters", iters)
	var total int64
	for it := 0; it < iters; it++ {
		var wg sync.WaitGroup
		for i := 0; i < 8; i++ {
			wg.Add(1)
			go func(n int) {
				defer wg.Done()
				if n%2 == 0 {
					time.Sleep(time.Millisecond)
				} else {
					runtime.Gosched()
				}
				atomic.AddInt64(&total, int64(n))
			}(i)
		}
		wg.Wait()
		if it%50 == 49 {
			fmt.Println("iteration", it+1, "total", atomic.LoadInt64(&total))
		}
	}
	fmt.Println("gostress done", total == int64(28*iters))
}
