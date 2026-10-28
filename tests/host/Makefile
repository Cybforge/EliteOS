# Host-side tests for the kernel's crypto, X.509 and image code:  make -C tests/host
# (also run by `make test` in the top directory). Needs gcc and the openssl CLI.

K       := ../../kernel
OUT     := ../../build/tests
CFLAGS  := -O2 -w -fno-builtin -Ishim -I$(K)/include -I$(K)/crypto -include stdint.h -include stdbool.h \
           -include stddef.h -include strings.h
CRYPTO  := $(K)/crypto/sha2.c $(K)/crypto/aes.c $(K)/crypto/chacha.c $(K)/crypto/x25519.c \
           $(K)/crypto/bignum.c $(K)/crypto/ec.c $(K)/crypto/rsa.c

.PHONY: all run
all: run

$(OUT):
	@mkdir -p $@

$(OUT)/crypto_vectors: crypto_vectors.c $(CRYPTO) | $(OUT)
	@gcc $(CFLAGS) -o $@ $< $(K)/crypto/sha2.c $(K)/crypto/aes.c $(K)/crypto/chacha.c $(K)/crypto/pbkdf2.c

$(OUT)/vectors.h: gen_vectors.py | $(OUT)
	@cd $(OUT) && python3 $(CURDIR)/gen_vectors.py > vectors.h

$(OUT)/pubkey: pubkey.c $(OUT)/vectors.h $(CRYPTO)
	@gcc $(CFLAGS) -I$(OUT) -o $@ $< $(K)/crypto/sha2.c $(K)/crypto/x25519.c $(K)/crypto/bignum.c $(K)/crypto/ec.c $(K)/crypto/rsa.c

$(OUT)/roots: roots.c $(K)/net/x509.c $(CRYPTO) | $(OUT)
	@gcc $(CFLAGS) -o $@ $< $(K)/net/x509.c $(K)/crypto/sha2.c $(K)/crypto/bignum.c $(K)/crypto/ec.c $(K)/crypto/rsa.c

$(OUT)/x509_chain: x509_chain.c $(K)/net/x509.c $(CRYPTO) | $(OUT)
	@gcc $(CFLAGS) -o $@ $< $(K)/net/x509.c $(K)/crypto/sha2.c $(K)/crypto/bignum.c $(K)/crypto/ec.c $(K)/crypto/rsa.c

run: $(OUT)/crypto_vectors $(OUT)/pubkey $(OUT)/roots $(OUT)/x509_chain
	@echo "== symmetric crypto" && $(OUT)/crypto_vectors | tail -1
	@echo "== public key crypto" && $(OUT)/pubkey | tail -1
	@echo "== root certificates" && $(OUT)/roots ../../root/etc/ssl/certs/ca-bundle.pem | tail -1
	@echo "== certificate chains" && mkdir -p $(OUT)/pki && cd $(OUT)/pki && sh $(CURDIR)/mkpki.sh >/dev/null 2>&1 && ../x509_chain | tail -1
