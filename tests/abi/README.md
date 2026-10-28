# System V program interface tests

These are ordinary static programs, built on a development machine with the host compiler and run inside ZenithOS
to check the program interface (memory, processes, threads, signals, files, poll/epoll, sockets).

```
gcc -static -O1 -o abitest abitest.c -lpthread
mcopy -o -i disk.img@@1M abitest ::/abitest      # any FAT disk the guest can mount
```

Start ZenithOS with that disk attached, open a terminal and run `/disk/abitest`. Every line starts with `ok`, `FAIL`
or `skip`; the last line reports the number of failures. The name-lookup and HTTP checks are skipped when the
resolver the guest was given has no way out to the internet.

Besides `abitest` the directory holds programs that behave like real software:

| program | what it exercises |
| --- | --- |
| `dyn.c` | a dynamically linked glibc program: PT_INTERP, `dlopen`, threads, fork + exec |
| `hello.go`, `gostress.go` | a static Go program: goroutines, the network poller (epoll), HTTP over the loopback, many threads |
| `hello_rs.rs` | a Rust program: threads, TCP over the loopback |
| `Hello.java` | the OpenJDK 21 runtime (HotSpot): threads, the class loader, garbage collection |
| `sigstress.c` | register and FPU state across thousands of signals, including Go-style injected calls |

`build.sh disk.img` builds all of them. The Java runtime is started through the dynamic loader because ZenithOS
does not ship one:

```
/disk/ld.so --library-path /disk/lib /disk/jre/bin/java -Xshare:off -XX:+UseSerialGC -Xmx128m -cp /disk Hello
```
