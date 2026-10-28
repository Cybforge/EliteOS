#!/bin/sh
# A test PKI: P-384 root, RSA intermediate, P-256 and RSA-PSS leaves, and an untrusted CA.
set -e
openssl ecparam -name secp384r1 -genkey -noout -out root.key
openssl req -x509 -new -key root.key -sha384 -days 3650 -subj "/CN=ZenithOS Test Root" -addext "basicConstraints=critical,CA:TRUE" -out root.pem
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out int.key
openssl req -new -key int.key -subj "/CN=ZenithOS Test Intermediate" -out int.csr
printf "basicConstraints=critical,CA:TRUE\n" > int.ext
openssl x509 -req -in int.csr -CA root.pem -CAkey root.key -CAcreateserial -days 1825 -sha384 -extfile int.ext -out int.pem
openssl ecparam -name prime256v1 -genkey -noout -out leaf.key
openssl req -new -key leaf.key -subj "/CN=localhost" -out leaf.csr
printf "basicConstraints=CA:FALSE\nsubjectAltName=DNS:localhost,DNS:*.zenith.test,IP:10.0.2.2\n" > leaf.ext
openssl x509 -req -in leaf.csr -CA int.pem -CAkey int.key -CAcreateserial -days 365 -sha256 -extfile leaf.ext -out leaf.pem
# a leaf signed directly by an untrusted CA
openssl ecparam -name prime256v1 -genkey -noout -out evil.key
openssl req -x509 -new -key evil.key -sha256 -days 30 -subj "/CN=Evil Root" -addext "basicConstraints=critical,CA:TRUE" -out evil.pem
openssl x509 -req -in leaf.csr -CA evil.pem -CAkey evil.key -CAcreateserial -days 30 -sha256 -extfile leaf.ext -out leaf_evil.pem
# an RSA-PSS leaf
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out pss.key
openssl req -new -key pss.key -subj "/CN=pss.zenith.test" -out pss.csr
openssl x509 -req -in pss.csr -CA int.pem -CAkey int.key -CAcreateserial -days 365 -sha256 -sigopt rsa_padding_mode:pss -sigopt rsa_pss_saltlen:32 -extfile leaf.ext -out leaf_pss.pem
