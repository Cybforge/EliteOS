package main

import (
	"fmt"
	"io"
	"net"
	"net/http"
	"runtime"
	"sync"
	"time"
)

func main() {
	fmt.Println("go hello", runtime.GOOS, runtime.GOARCH, "cpus", runtime.NumCPU())
	var wg sync.WaitGroup
	sum := 0
	var mu sync.Mutex
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(n int) {
			defer wg.Done()
			time.Sleep(10 * time.Millisecond)
			mu.Lock()
			sum += n
			mu.Unlock()
		}(i)
	}
	wg.Wait()
	fmt.Println("goroutines sum", sum)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		fmt.Println("listen:", err)
		return
	}
	go http.Serve(ln, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, "served by go on zenith") }))
	resp, err := http.Get("http://" + ln.Addr().String() + "/")
	if err != nil {
		fmt.Println("get:", err)
		return
	}
	b, _ := io.ReadAll(resp.Body)
	fmt.Println("http over loopback:", string(b))
}
