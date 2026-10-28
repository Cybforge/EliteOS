import subprocess, os
def run(*a, inp=None): return subprocess.run(a, check=True, capture_output=True, input=inp).stdout
def h(b): return '"' + b.hex() + '"'
def der_items(b):
    # minimal DER walker: returns list of (tag, value) of a SEQUENCE's children
    assert b[0] == 0x30
    i = 1; l = b[i]; i += 1
    if l & 0x80: n = l & 0x7f; l = int.from_bytes(b[i:i+n], 'big'); i += n
    end = i + l; out = []
    while i < end:
        t = b[i]; i += 1; l = b[i]; i += 1
        if l & 0x80: n = l & 0x7f; l = int.from_bytes(b[i:i+n], 'big'); i += n
        out.append((t, b[i:i+l])); i += l
    return out
msg = b"ZenithOS signs this message"
open('msg.txt','wb').write(msg)
print('static const char *msg = "%s";' % msg.decode())
print("static const struct { int bits, hash; const char *n, *pkcs1, *pss; } rsa_vec[] = {")
for bits in (2048, 4096):
    run('openssl','genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:%d' % bits,'-out','rsa.pem')
    n = bytes.fromhex(run('openssl','rsa','-in','rsa.pem','-noout','-modulus').decode().strip().split('=')[1])
    for hn in ('256','384'):
        s1 = run('openssl','dgst','-sha'+hn,'-sign','rsa.pem','msg.txt')
        s2 = run('openssl','dgst','-sha'+hn,'-sign','rsa.pem','-sigopt','rsa_padding_mode:pss','-sigopt','rsa_pss_saltlen:digest','msg.txt')
        print('  { %d, %s, %s, %s, %s },' % (bits, hn, h(n), h(s1), h(s2)))
print("};")
print("static const struct { int curve; const char *pub, *r, *s; int hash; } ec_vec[] = {")
for cid, name, L, hn in ((0,'prime256v1',32,'256'),(1,'secp384r1',48,'384'),(0,'prime256v1',32,'384'),(1,'secp384r1',48,'256')):
    run('openssl','ecparam','-name',name,'-genkey','-noout','-out','ec.pem')
    pub = run('openssl','ec','-in','ec.pem','-pubout','-outform','DER')[-(2*L+1):]
    sig = run('openssl','dgst','-sha'+hn,'-sign','ec.pem','msg.txt')
    (_, r), (_, s) = der_items(sig)
    r = int.from_bytes(r,'big').to_bytes(L,'big'); s = int.from_bytes(s,'big').to_bytes(L,'big')
    print('  { %d, %s, %s, %s, %s },' % (cid, h(pub), h(r), h(s), hn))
print("};")
print("static const struct { int curve; const char *priv, *peer, *shared; } ecdh_vec[] = {")
for cid, name, L in ((0,'prime256v1',32),(1,'secp384r1',48)):
    run('openssl','ecparam','-name',name,'-genkey','-noout','-out','a.pem')
    run('openssl','ecparam','-name',name,'-genkey','-noout','-out','b.pem')
    run('openssl','ec','-in','b.pem','-pubout','-out','bpub.pem')
    shared = run('openssl','pkeyutl','-derive','-inkey','a.pem','-peerkey','bpub.pem')
    items = der_items(run('openssl','ec','-in','a.pem','-outform','DER'))
    priv = items[1][1]
    pb = run('openssl','ec','-in','b.pem','-pubout','-outform','DER')[-(2*L+1):]
    print('  { %d, %s, %s, %s },' % (cid, h(priv), h(pb), h(shared)))
print("};")
