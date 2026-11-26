#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Published test vectors and differential tests for libsig's lowest layers.
#   python3 -I libsig/tests/vectors.py SIGTOOL VECDIR [SECONDS]
# SIGTOOL is libsig/tests/sigtool built for the host; VECDIR holds what
# fetch-vectors.sh downloads (a missing file skips its section, and says so).
# SECONDS bounds the random differential part (default 20).
# Prints one line per section and "vectors: N checks, M failed".
import hashlib, json, os, random, re, subprocess, sys, time

tool, vec = sys.argv[1], sys.argv[2]
budget = float(sys.argv[3]) if len(sys.argv) > 3 else 20.0
p = subprocess.Popen([tool, "batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
checks = fails = 0

def ask(line):
    p.stdin.write(line + "\n"); p.stdin.flush()
    return p.stdout.readline().strip()

def check(ok, what):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print("FAIL:", what)

def h(b): return b.hex() if b else "-"

def path(name):
    f = os.path.join(vec, name)
    if not os.path.exists(f):
        print(f"SKIP {name}: not downloaded (run libsig/tests/fetch-vectors.sh)")
        return None
    return f

# ---- hashes: FIPS 180-4 examples, then random inputs against Python's hashlib
fips = {
    "sha1": [(b"abc", "a9993e364706816aba3e25717850c26c9cd0d89d")],
    "sha384": [(b"abc", "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"),
               (b"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
                "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039")],
    "sha512": [(b"abc", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f")],
    "sha256": [(b"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")],
}
n0 = checks
for alg, vs in fips.items():
    for m, d in vs:
        check(ask(f"{alg} {h(m)}") == d, f"{alg} FIPS example {m[:10]!r}")
rng = random.Random(1)
for i in range(300):
    m = bytes(rng.randrange(256) for _ in range(rng.choice([0, 1, 55, 56, 63, 64, 111, 112, 127, 128, rng.randrange(1, 600)])))
    for alg in ("sha1", "sha256", "sha384", "sha512"):
        check(ask(f"{alg} {h(m)}") == hashlib.new(alg, m).hexdigest(), f"{alg} random length {len(m)}")
print(f"hashes: {checks - n0} checks")

# ---- CRC-24 (RFC 9580 6.1): against a direct transcription of the RFC's description
def crc24(b):
    c = 0xB704CE
    for x in b:
        c ^= x << 16
        for _ in range(8):
            c <<= 1
            if c & 0x1000000: c ^= 0x1864CFB
    return c & 0xFFFFFF
n0 = checks
check(ask("crc24 -") == "b704ce", "crc24 of nothing is the initial value")
for i in range(200):
    m = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 300)))
    check(ask(f"crc24 {h(m)}") == f"{crc24(m):06x}", "crc24 random")
print(f"crc24: {checks - n0} checks")

# ---- RSA PKCS#1 v1.5 (Project Wycheproof)
hmap = {"SHA-256": (8, hashlib.sha256), "SHA-384": (9, hashlib.sha384), "SHA-512": (10, hashlib.sha512)}
acceptable = {}
for f in ("rsa_signature_2048_sha256_test.json", "rsa_signature_2048_sha512_test.json", "rsa_signature_3072_sha256_test.json",
          "rsa_signature_3072_sha512_test.json", "rsa_signature_4096_sha512_test.json"):
    fp = path(f)
    if not fp: continue
    d = json.load(open(fp))
    n0 = checks; nv = ni = 0
    for g in d["testGroups"]:
        n, e = g["publicKey"]["modulus"], g["publicKey"]["publicExponent"]
        algo, hf = hmap[g["sha"]]
        for t in g["tests"]:
            dig = hf(bytes.fromhex(t["msg"])).hexdigest()
            r = ask(f"rsa {n} {e} {t['sig'] or '-'} {algo} {dig}")
            if t["result"] == "valid":
                nv += 1; check(r == "1", f"{f} tc {t['tcId']} valid but got {r}")
            elif t["result"] == "invalid":
                ni += 1; check(r != "1", f"{f} tc {t['tcId']} invalid ({t['comment']}, {t['flags']}) but accepted")
            else:
                acceptable[f"{f}:{t['tcId']}"] = r
    print(f"{f}: {checks - n0} checks ({nv} valid accepted, {ni} invalid refused)")
if acceptable:
    print(f"wycheproof 'acceptable' cases (either answer allowed): {sum(1 for v in acceptable.values() if v == '1')} accepted, "
          f"{sum(1 for v in acceptable.values() if v != '1')} refused")

# ---- RSA forgeries a parsing verifier accepts, made here with a fresh key
try:
    from cryptography.hazmat.primitives.asymmetric import rsa, padding
    from cryptography.hazmat.primitives import hashes
    have_crypto = True
except ImportError:
    have_crypto = False
    print("SKIP RSA forgeries: Python 'cryptography' is not installed")
DI256 = bytes.fromhex("3031300d060960864801650304020105000420")
def icbrt(x):
    lo, hi = 0, 1 << ((x.bit_length() + 2) // 3 + 1)
    while lo < hi:
        mid = (lo + hi) // 2
        if mid ** 3 < x: lo = mid + 1
        else: hi = mid
    return lo
def naive_verify(n, e, s, digest):
    """The classic broken verifier: skip 00 01 FF.. 00, read a DigestInfo, ignore what follows."""
    k = (n.bit_length() + 7) // 8
    em = pow(s, e, n).to_bytes(k, "big")
    if em[:2] != b"\x00\x01": return False
    i = 2
    while i < k and em[i] == 0xFF: i += 1
    if i >= k or em[i] != 0: return False
    i += 1
    return em[i:i + len(DI256)] == DI256 and em[i + len(DI256):i + len(DI256) + 32] == digest
if have_crypto:
    n0 = checks
    for e in (3, 65537):
        key = rsa.generate_private_key(public_exponent=e, key_size=3072)
        pub = key.public_key().public_numbers()
        n, d = pub.n, key.private_numbers().d
        k = (n.bit_length() + 7) // 8
        nh, eh = f"{n:x}", f"{e:x}"
        nh, eh = nh.rjust(len(nh) + len(nh) % 2, "0"), eh.rjust(len(eh) + len(eh) % 2, "0")
        msg = b"Release file"
        dig = hashlib.sha256(msg).digest()
        good = key.sign(msg, padding.PKCS1v15(), hashes.SHA256())
        check(ask(f"rsa {nh} {eh} {good.hex()} 8 {dig.hex()}") == "1", f"e={e} genuine signature")
        def signraw(em): return pow(int.from_bytes(em, "big"), d, n).to_bytes(k, "big").hex()
        cases = {
            "garbage after the digest (shorter padding)": b"\x00\x01" + b"\xff" * 8 + b"\x00" + DI256 + dig + bytes((7 * i + 1) & 255 for i in range(k - 11 - len(DI256) - 32)),
            "a byte in the padding is not FF": b"\x00\x01" + b"\xff" * 5 + b"\xfe" + b"\xff" * (k - 3 - len(DI256) - 32 - 6) + b"\x00" + DI256 + dig,
            "block type 2": b"\x00\x02" + b"\xff" * (k - 3 - len(DI256) - 32) + b"\x00" + DI256 + dig,
            "DigestInfo without the NULL parameters": b"\x00\x01" + b"\xff" * (k - 3 - 17 - 32) + b"\x00" + bytes.fromhex("302f300b0609608648016503040201") + b"\x04\x20" + dig,
            "DigestInfo of SHA-1 shape with a SHA-256 value": b"\x00\x01" + b"\xff" * (k - 3 - 15 - 32) + b"\x00" + bytes.fromhex("3021300906052b0e03021a05000414") + dig,
            "DigestInfo with a wrong outer length": b"\x00\x01" + b"\xff" * (k - 3 - len(DI256) - 32) + b"\x00" + b"\x30\x32" + DI256[2:] + dig,
            "only 7 bytes of padding": b"\x00\x01" + b"\xff" * 7 + b"\x00" + DI256 + dig + b"\x00" * (k - 10 - len(DI256) - 32),
        }
        for name, em in cases.items():
            em = em[:k].ljust(k, b"\x00")
            check(ask(f"rsa {nh} {eh} {signraw(em)} 8 {dig.hex()}") == "0", f"e={e}: {name} (signed with the real key) refused")
        # signature value s >= n, and s + n
        check(ask(f"rsa {nh} {eh} {(int.from_bytes(good, 'big') + n):x} 8 {dig.hex()}") != "1", f"e={e}: s + n refused")
        check(ask(f"rsa {nh} {eh} {n:x} 8 {dig.hex()}") != "1", f"e={e}: s = n refused")
        check(ask(f"rsa {nh} {eh} - 8 {dig.hex()}") != "1", f"e={e}: empty signature refused")
        if e == 3:
            # Bleichenbacher 2006: no private key, only a cube root. Prefix 00 01 FF 00 DigestInfo digest, the rest free.
            prefix = b"\x00\x01\xff\x00" + DI256 + dig
            free = k - len(prefix)
            lo = int.from_bytes(prefix + b"\x00" * free, "big")
            hi = int.from_bytes(prefix + b"\xff" * free, "big")
            s = icbrt(lo)
            forged = s ** 3 <= hi
            check(forged and naive_verify(n, 3, s, dig), "the e=3 forgery is accepted by the naive parsing verifier (the test is meaningful)")
            check(ask(f"rsa {nh} {eh} {s:x} 8 {dig.hex()}") == "0", "e=3 Bleichenbacher forgery (cube root, garbage after the digest) refused")
            # small signature: s^3 < n with a correctly padded but short message does not exist; s = cube root of 'digest only'
            s2 = icbrt(int.from_bytes(DI256 + dig, "big"))
            check(ask(f"rsa {nh} {eh} {s2:x} 8 {dig.hex()}") == "0", "e=3 small signature (no padding, no wrap) refused")
    # key sanity
    check(ask(f"rsa {'f' * 511}e 010001 01 8 {'00' * 32}") == "-1", "even modulus refused as a key")
    check(ask(f"rsa {'f' * 512} 01 01 8 {'00' * 32}") == "-1", "exponent 1 refused as a key")
    check(ask(f"rsa {'f' * 512} 010000 01 8 {'00' * 32}") == "-1", "even exponent refused as a key")
    check(ask(f"rsa {'f' * 512} 010001 01 2 {'00' * 20}") == "-1", "SHA-1 refused")
    check(ask(f"rsa {'f' * 512} 010001 01 1 {'00' * 16}") == "-1", "MD5 refused")
    print(f"rsa forgeries and key checks: {checks - n0} checks")

# ---- Ed25519: RFC 8032 section 7.1
fp = path("rfc8032.txt")
if fp:
    txt = open(fp).read()
    sec = txt[txt.rindex("\n7.1.  Test Vectors for Ed25519\n"):txt.rindex("\n7.2.  Test Vectors for Ed25519ctx\n")]
    n0 = checks
    for block in re.split(r"-----TEST [^\n]*", sec)[1:]:
        fields, cur, mlen = {}, None, None
        for ln in block.split("\n"):
            t = ln.strip()
            if t.endswith(":") or t.startswith("MESSAGE ("):
                cur = t.split(" (")[0].rstrip(":"); fields[cur] = ""
                m = re.search(r"length (\d+) bytes?", t)
                if m: mlen = int(m.group(1))
            elif cur and re.fullmatch(r"[0-9a-f]+", t):
                fields[cur] += t
        pk, msg, sg = fields.get("PUBLIC KEY", ""), fields.get("MESSAGE", ""), fields.get("SIGNATURE", "")
        if mlen is None or len(msg) != 2 * mlen:
            check(False, f"RFC 8032 block parse: message length {len(msg) // 2} != {mlen}"); continue
        if len(pk) != 64 or len(sg) != 128: continue
        check(ask(f"ed {pk} {msg or '-'} {sg}") == "1", f"RFC 8032 vector pk {pk[:8]}")
        bad = bytearray(bytes.fromhex(sg)); bad[0] ^= 1
        check(ask(f"ed {pk} {msg or '-'} {bad.hex()}") == "0", f"RFC 8032 vector pk {pk[:8]} with R changed")
        bad = bytearray(bytes.fromhex(sg)); bad[40] ^= 1
        check(ask(f"ed {pk} {msg or '-'} {bad.hex()}") == "0", f"RFC 8032 vector pk {pk[:8]} with S changed")
        m2 = bytes.fromhex(msg) + b"x" if msg else b"x"
        check(ask(f"ed {pk} {m2.hex()} {sg}") == "0", f"RFC 8032 vector pk {pk[:8]} with the message changed")
        # S + L: the same point equation, refused because S >= L
        L = 2**252 + 27742317777372353535851937790883648493
        S = int.from_bytes(bytes.fromhex(sg)[32:], "little") + L
        if S < 2**256:
            check(ask(f"ed {pk} {msg or '-'} {sg[:64]}{S.to_bytes(32, 'little').hex()}") == "0", f"RFC 8032 vector pk {pk[:8]} with S + L (non-canonical S)")
    print(f"rfc8032: {checks - n0} checks")

# ---- RFC 9580 A.1 and A.2: a version 4 Ed25519Legacy signature
fp = path("rfc9580.txt")
if fp:
    n0 = checks
    q = "3f098994bdd916ed4053197934e4a87c80733a1280d62f8010992e43ee3b2406"
    d = "f6220a3f757814f4c2176ffbb68b00249cd4ccdc059c4b34ad871f30b1740280"
    r = "56f90cca98e2102637bd983fdb16c131dfd27ed82bf4dde5606e0d756aed3366"
    s = "d09c4fa11527f038e0f57f2201d82f2ea2c9033265fa6ceb489e854bae61b404"
    txt = open(fp).read()
    check(all(x in txt.replace(" ", "") for x in (q, d, r, s)), "the RFC 9580 A.2 values are in the downloaded RFC")
    check(hashlib.sha256(bytes.fromhex("4f70656e504750040016080006050255f95f9504ff0000000c")).hexdigest() == d, "RFC 9580 A.2 digest")
    check(ask(f"ed {q} {d} {r}{s}") == "1", "RFC 9580 A.2 signature")
    print(f"rfc9580 A.2: {checks - n0} checks")

# ---- Ed25519: Project Wycheproof
fp = path("ed25519_test.json")
if fp:
    dd = json.load(open(fp))
    n0 = checks; nv = ni = 0
    for g in dd["testGroups"]:
        pk = g["publicKey"]["pk"]
        small = ask(f"small {pk}") == "1"
        for t in g["tests"]:
            r = ask(f"ed {pk} {t['msg'] or '-'} {t['sig'] if len(t['sig']) == 128 else '00' * 64}") if len(t["sig"]) == 128 else "0"
            if small: r = "0"   # the keyring refuses small-order keys
            if t["result"] == "valid":
                nv += 1; check(r == "1", f"wycheproof ed25519 tc {t['tcId']} valid but refused")
            elif t["result"] == "invalid":
                ni += 1; check(r == "0", f"wycheproof ed25519 tc {t['tcId']} invalid ({t['comment']}) but accepted")
    print(f"ed25519_test.json: {checks - n0} checks ({nv} valid, {ni} invalid)")

# ---- Ed25519 edge cases (ed25519-speccheck, cases 0 to 11)
fp = path("speccheck-cases.json")
if fp:
    cases = json.load(open(fp))
    got = []
    for c in cases:
        if ask(f"small {c['pub_key']}") == "1": got.append(0)   # small-order keys are refused at load time
        else: got.append(int(ask(f"ed {c['pub_key']} {c['message']} {c['signature']}")))
    # What this verifier must answer (cofactorless, S < L, canonical A and R,
    # small-order A refused): only cases 2 and 3 pass. 0 and 1: small-order A.
    # 4 and 5: pass only the cofactored equation. 6 and 7: S >= L. 8 and 9:
    # non-canonical R. 10 and 11: non-canonical A.
    want = [0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0]
    for i, (g, w) in enumerate(zip(got, want)):
        check(g == w, f"speccheck case {i}: got {g}, want {w}")
    print(f"speccheck: {len(cases)} cases, results {''.join(map(str, got))}")

# ---- modular exponentiation: Montgomery and the slow version against Python integers
n0 = checks
t0 = time.time()
it = 0
while time.time() - t0 < budget:
    it += 1
    bits = rng.choice([1, 2, 31, 32, 33, 63, 64, 65, 127, 128, 255, 256, 511, 1024, 2047, 2048, 3072, 4096, rng.randrange(2, 4097)])
    m = rng.getrandbits(bits) | 1 | (1 << (bits - 1))
    b = rng.randrange(0, m)
    if rng.random() < 0.1: b = rng.choice([0, 1, m - 1]) % m
    e = rng.choice([3, 65537, 1, 0, rng.getrandbits(rng.randrange(1, 70)), rng.getrandbits(min(bits, 300))])
    want = pow(b, e, m)
    k = (m.bit_length() + 7) // 8
    eh = f"{e:x}" if e else "00"
    eh = eh if len(eh) % 2 == 0 else "0" + eh  # whole bytes
    bh = f"{b:x}".rjust(2 * k, "0")
    mh = f"{m:x}".rjust(2 * k, "0")
    got = ask(f"modexp {bh} {eh} {mh}")
    check(got != "err" and int(got, 16) == want, f"modexp {bits} bits e={e}")
    if bits <= 1024 and e.bit_length() <= 64:
        got2 = ask(f"slowexp {bh} {eh} {mh}")
        check(got2 != "err" and int(got2, 16) == want, f"slowexp {bits} bits")
    if it % 50 == 0:   # out of range: b >= m must be refused
        check(ask(f"modexp {f'{m:x}'.rjust(2 * k, '0')} 03 {mh}") == "err", "modexp with b = m refused")
print(f"modexp differential: {checks - n0} checks in {time.time() - t0:.1f} s")

p.stdin.close(); p.wait()
print(f"vectors: {checks} checks, {fails} failed")
sys.exit(1 if fails else 0)
