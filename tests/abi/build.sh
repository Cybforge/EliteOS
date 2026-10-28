#!/bin/sh
# Builds the test programs for the System V program interface and puts them on a FAT disk image that ZenithOS can
# mount (the image needs a partition starting at 1 MiB; the host's own C library is copied in for the dynamic ones).
#   tests/abi/build.sh disk.img
set -e
img=${1:?usage: build.sh disk.img}
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
cd "$here"
gcc -static -O1 -w -o "$out/abitest" abitest.c -lpthread
gcc -static -O1 -w -o "$out/hello" hello.c
gcc -static -O1 -w -o "$out/sigstress" sigstress.c -lpthread
gcc -O1 -w -o "$out/dyn" dyn.c -lm -ldl -lpthread -Wl,--dynamic-linker=/disk/ld.so -Wl,-rpath,/disk/lib
CGO_ENABLED=0 go build -o "$out/hello_go" hello.go
CGO_ENABLED=0 go build -o "$out/gostress" gostress.go
rustc -O -C link-arg=-Wl,--dynamic-linker=/disk/ld.so -C link-arg=-Wl,-rpath,/disk/lib -o "$out/hello_rs" hello_rs.rs
JAVA_TOOL_OPTIONS= javac -d "$out" Hello.java
JAVA_TOOL_OPTIONS= jlink --add-modules java.base --output "$out/jre" --strip-debug --no-header-files --no-man-pages --compress=zip-6
libc=/lib/x86_64-linux-gnu
mmd -i "$img@@1M" ::/lib 2>/dev/null || true
mcopy -o -i "$img@@1M" /lib64/ld-linux-x86-64.so.2 ::/ld.so
for l in libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 libgcc_s.so.1 libstdc++.so.6 libz.so.1; do
  mcopy -o -i "$img@@1M" "$libc/$l" "::/lib/$l"
done
for f in abitest hello sigstress dyn hello_go gostress hello_rs Hello.class; do mcopy -o -i "$img@@1M" "$out/$f" "::/$f"; done
mcopy -s -o -i "$img@@1M" "$out/jre" ::/jre
rm -rf "$out"
