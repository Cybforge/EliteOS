#!/usr/bin/env python3
"""Build and sign a ZenithOS package repository.

    mkstore.py keygen <key.pem> <key.pub>
        make a signing key; the .pub file is what systems trust (it goes to
        /etc/zenith/keys), the .pem file must stay secret

    mkstore.py build <specs-dir> <repo-dir> <key.pem>
        turn every <specs-dir>/*.pkg into <repo-dir>/pkg/<name>_<version>.zpk,
        write <repo-dir>/Packages and sign it (Packages.sig)

    mkstore.py verify <repo-dir> <key.pub>
        check a repository's signature and checksums

A spec is a package stanza ("Key: value" lines, see kernel/include/pkg.h) plus
any number of "Payload: <source path>=<path in the package>" lines. A source
that is a directory is added recursively. Paths are relative to the current
directory.
"""
import base64
import gzip
import hashlib
import io
import os
import sys
import tarfile

import subprocess


def read_stanza(path):
    fields = []
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line.strip() or line.startswith('#'):
            continue
        key, _, val = line.partition(':')
        fields.append((key.strip(), val.strip()))
    return fields


def openssl(*args, data=None):
    return subprocess.run(['openssl', *args], input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout


def der_ints(sig):
    """the two integers of a DER ECDSA signature"""
    assert sig[0] == 0x30
    i = 2 if sig[1] < 0x80 else 2 + (sig[1] & 0x7F)
    out = []
    for _ in range(2):
        assert sig[i] == 0x02
        n = sig[i + 1]
        out.append(int.from_bytes(sig[i + 2:i + 2 + n], 'big'))
        i += 2 + n
    return out


def der_sig(r, s):
    def enc(v):
        b = v.to_bytes((v.bit_length() + 8) // 8, 'big')
        return bytes([0x02, len(b)]) + b
    body = enc(r) + enc(s)
    return bytes([0x30, len(body)]) + body


def keygen(pem_path, pub_path):
    openssl('ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', pem_path)
    os.chmod(pem_path, 0o600)
    der = openssl('ec', '-in', pem_path, '-pubout', '-outform', 'DER')
    point = der[-65:]                                   # the uncompressed point closes the SubjectPublicKeyInfo
    assert point[0] == 4
    with open(pub_path, 'w') as f:
        f.write('zenith-key-v1 ' + base64.b64encode(point).decode() + '\n')


def load_key(pem_path):
    return pem_path


def sign(data, key):
    r, s = der_ints(openssl('dgst', '-sha256', '-sign', key, data=data))
    return 'zenith-sig-v1 ' + base64.b64encode(r.to_bytes(32, 'big') + s.to_bytes(32, 'big')).decode() + '\n'


def verify(data, sig_text, pub_text):
    sig = base64.b64decode(sig_text.split()[1])
    point = base64.b64decode(pub_text.split()[1])
    # SubjectPublicKeyInfo for prime256v1 followed by the point
    spki = bytes.fromhex('3059301306072a8648ce3d020106082a8648ce3d030107034200') + point
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        open(d + '/pub.der', 'wb').write(spki)
        open(d + '/sig.der', 'wb').write(der_sig(int.from_bytes(sig[:32], 'big'), int.from_bytes(sig[32:], 'big')))
        openssl('dgst', '-sha256', '-verify', d + '/pub.der', '-keyform', 'DER', '-signature', d + '/sig.der', data=data)


def add_payload(tar, src, dst):
    if os.path.isdir(src):
        for root, dirs, files in os.walk(src):
            dirs.sort()
            for name in sorted(files):
                full = os.path.join(root, name)
                add_payload(tar, full, os.path.join(dst, os.path.relpath(full, src)))
        return 0
    data = open(src, 'rb').read()
    info = tarfile.TarInfo(dst.replace(os.sep, '/'))
    info.size = len(data)
    info.mode = 0o755 if os.access(src, os.X_OK) else 0o644
    tar.addfile(info, io.BytesIO(data))
    return len(data)


def build_package(spec_path, repo):
    fields = read_stanza(spec_path)
    meta = [(k, v) for k, v in fields if k != 'Payload']
    payload = [v for k, v in fields if k == 'Payload']
    name = dict(meta)['Package']
    version = dict(meta)['Version']

    manifest = ''.join(f'{k}: {v}\n' for k, v in meta) + '\n'
    raw = io.BytesIO()
    installed = 0
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        info = tarfile.TarInfo('MANIFEST')
        info.size = len(manifest.encode())
        info.mode = 0o644
        tar.addfile(info, io.BytesIO(manifest.encode()))
        for p in payload:
            src, _, dst = p.partition('=')
            installed += add_payload(tar, src.strip(), (dst or os.path.basename(src)).strip())
    # a gzip stream with no timestamp, so a rebuild of unchanged input is byte for byte the same
    blob = gzip.compress(raw.getvalue(), 9, mtime=0)
    os.makedirs(os.path.join(repo, 'pkg'), exist_ok=True)
    fname = f'pkg/{name}_{version}.zpk'
    open(os.path.join(repo, fname), 'wb').write(blob)

    stanza = [(k, v) for k, v in meta]
    stanza += [('Filename', fname), ('Size', str(len(blob))), ('Installed-Size', str(installed)),
               ('SHA256', hashlib.sha256(blob).hexdigest())]
    return ''.join(f'{k}: {v}\n' for k, v in stanza) + '\n'


def build(specs, repo, key_path):
    key = load_key(key_path)
    os.makedirs(repo, exist_ok=True)
    for old in os.listdir(os.path.join(repo, 'pkg')) if os.path.isdir(os.path.join(repo, 'pkg')) else []:
        os.remove(os.path.join(repo, 'pkg', old))
    index = ''
    for spec in sorted(os.listdir(specs)):
        if spec.endswith('.pkg'):
            index += build_package(os.path.join(specs, spec), repo)
    data = index.encode()
    open(os.path.join(repo, 'Packages'), 'wb').write(data)
    open(os.path.join(repo, 'Packages.sig'), 'w').write(sign(data, key))
    print(f'{repo}: {index.count("Package:")} package(s), index signed')


def check(repo, pub_path):
    data = open(os.path.join(repo, 'Packages'), 'rb').read()
    verify(data, open(os.path.join(repo, 'Packages.sig')).read(), open(pub_path).read())
    for block in data.decode().split('\n\n'):
        f = dict(l.split(': ', 1) for l in block.splitlines() if ': ' in l)
        if 'Filename' in f:
            blob = open(os.path.join(repo, f['Filename']), 'rb').read()
            assert hashlib.sha256(blob).hexdigest() == f['SHA256'], f['Filename']
            assert len(blob) == int(f['Size']), f['Filename']
    print(f'{repo}: signature and checksums are good')


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == 'keygen':
        keygen(a[1], a[2])
    elif len(a) == 4 and a[0] == 'build':
        build(a[1], a[2], a[3])
    elif len(a) == 3 and a[0] == 'verify':
        check(a[1], a[2])
    else:
        sys.exit(__doc__)
