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
