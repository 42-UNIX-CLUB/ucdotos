#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# OpenPGP structure, policy and cleartext attack cases for libsig.
#   python3 -I libsig/tests/pgpcases.py SIGTOOL OUTDIR [GPGV]
# Builds keys, keyrings and signed files with a small OpenPGP WRITER that lives
# only here (test code; it uses Python's 'cryptography' package to sign), runs
# sigtool on each and checks the outcome. Files go to OUTDIR/cases/. With GPGV
# (the host's gpgv), every file this script expects to verify is also given to
# gpgv, so the writer is checked against an independent implementation.
# Prints "pgpcases: N checks, M failed".
import hashlib, os, struct, subprocess, sys, base64

from cryptography.hazmat.primitives.asymmetric import rsa, ed25519, padding, utils
from cryptography.hazmat.primitives import hashes, serialization

tool, out = sys.argv[1], sys.argv[2]
gpgv = sys.argv[3] if len(sys.argv) > 3 else None
D = os.path.join(out, "cases")
os.makedirs(D, exist_ok=True)
checks = fails = 0
T0 = 1700000000          # keys are made at this time
NOW = 1790000000         # and checked at this one unless a case says otherwise

def check(ok, what):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print("FAIL:", what)

# ---------------- the writer ----------------
def pkt(tag, body, old=False):
    n = len(body)
    if old:
        if n < 256: return bytes([0x80 | tag << 2 | 0, n]) + body
        if n < 65536: return bytes([0x80 | tag << 2 | 1]) + struct.pack(">H", n) + body
        return bytes([0x80 | tag << 2 | 2]) + struct.pack(">I", n) + body
    if n < 192: h = bytes([n])
    elif n < 8384: n2 = n - 192; h = bytes([(n2 >> 8) + 192, n2 & 255])
    else: h = b"\xff" + struct.pack(">I", n)
    return bytes([0xc0 | tag]) + h + body

def mpi(x):
    if isinstance(x, bytes): x = int.from_bytes(x, "big")
    b = x.to_bytes((x.bit_length() + 7) // 8, "big")
    return struct.pack(">H", x.bit_length()) + b

def sp(t, data, crit=False):
    body = bytes([t | (0x80 if crit else 0)]) + data
    n = len(body)
    if n < 192: return bytes([n]) + body
    n2 = n - 192
    return bytes([(n2 >> 8) + 192, n2 & 255]) + body

HASH = {1: hashlib.md5, 2: hashlib.sha1, 8: hashlib.sha256, 9: hashlib.sha384, 10: hashlib.sha512}
CHASH = {1: hashes.MD5(), 2: hashes.SHA1(), 8: hashes.SHA256(), 9: hashes.SHA384(), 10: hashes.SHA512()}

class Key:
    def __init__(self, kind, created=T0, bits=3072, priv=None):
        self.kind, self.created = kind, created
        if kind == "rsa":
            self.priv = priv or rsa.generate_private_key(public_exponent=65537, key_size=bits)
            pn = self.priv.public_key().public_numbers()
            self.algo = 1
            mat = mpi(pn.n) + mpi(pn.e)
        else:
            self.priv = priv or ed25519.Ed25519PrivateKey.generate()
            raw = self.priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
            if kind == "ed":       # EdDSA legacy, algo 22
                self.algo = 22
                oid = bytes.fromhex("2b06010401da470f01")
                mat = bytes([len(oid)]) + oid + mpi(b"\x40" + raw)
            else:                  # native Ed25519, algo 27
                self.algo = 27
                mat = raw
        self.body = bytes([4]) + struct.pack(">I", created) + bytes([self.algo]) + mat
        self.fpr = hashlib.sha1(b"\x99" + struct.pack(">H", len(self.body)) + self.body).digest()
        self.keyid = self.fpr[12:]

    def hashed_key(self): return b"\x99" + struct.pack(">H", len(self.body)) + self.body
    def packet(self, sub=False): return pkt(14 if sub else 6, self.body)

def sign(key, sigtype, data, halgo=8, created=None, hashed=None, unhashed=None, issuer=True, version=4,
         corrupt=False, pkalgo=None, left16=None):
    """A signature packet body over data (bytes already in hashing form)."""
    created = key.created + 10 if created is None else created
    hsp = (sp(33, b"\x04" + key.fpr) + sp(2, struct.pack(">I", created))) if issuer is True else (sp(2, struct.pack(">I", created)) if issuer == "nofpr" else b"")
    if issuer == "nofpr": hsp = sp(2, struct.pack(">I", created))
    if hashed is not None: hsp += hashed
    usp = (sp(16, key.keyid) if issuer else b"") + (unhashed or b"")
    pa = key.algo if pkalgo is None else pkalgo
    head = bytes([version, sigtype, pa, halgo]) + struct.pack(">H", len(hsp)) + hsp
    trailer = bytes([4, 0xff]) + struct.pack(">I", len(head))
    dig = HASH[halgo](data + head + trailer).digest()
    if key.kind == "rsa":
        s = key.priv.sign(dig, padding.PKCS1v15(), utils.Prehashed(CHASH[halgo]))
        if corrupt: s = bytes([s[0] ^ 1]) + s[1:]
        mat = mpi(s)
    else:
        s = key.priv.sign(dig)
        if corrupt: s = s[:40] + bytes([s[40] ^ 1]) + s[41:]
        mat = (mpi(s[:32]) + mpi(s[32:])) if key.kind == "ed" else s
    l16 = dig[:2] if left16 is None else left16
    return head + struct.pack(">H", len(usp)) + usp + l16 + mat

def uidpart(uid): return b"\xb4" + struct.pack(">I", len(uid)) + uid

def tpk(P, uid=b"Test Key <test@example.invalid>", flags=b"\x03", keyexp=None, selfhash=8, selfextra=b"", subs=(),
        revoke=False, revoke_bad=False, uid_revoked=False, selfcorrupt=False, secret=False):
    h = (sp(27, flags) if flags is not None else b"") + (sp(9, struct.pack(">I", keyexp)) if keyexp else b"") + selfextra
    out = P.packet()
    if revoke or revoke_bad:
        out += pkt(2, sign(P, 0x20, P.hashed_key(), corrupt=revoke_bad, created=P.created + 100))
    out += pkt(13, uid)
    out += pkt(2, sign(P, 0x13, P.hashed_key() + uidpart(uid), halgo=selfhash, hashed=h, corrupt=selfcorrupt))
    if uid_revoked:
        out += pkt(2, sign(P, 0x30, P.hashed_key() + uidpart(uid), created=P.created + 50))
    for s in subs:
        out += s
    return out

def subkey(P, S, flags=b"\x02", backsig=True, backsig_by=None, keyexp=None, bindcorrupt=False, revoke=False, bindhash=8):
    data = P.hashed_key() + S.hashed_key()
    emb = b""
    if backsig:
        bs = sign(backsig_by or S, 0x19, data)
        emb = sp(32, bs)
    h = (sp(27, flags) if flags is not None else b"") + (sp(9, struct.pack(">I", keyexp)) if keyexp else b"")
    out = S.packet(sub=True) + pkt(2, sign(P, 0x18, data, hashed=h, unhashed=emb, corrupt=bindcorrupt, halgo=bindhash))
    if revoke:
        out += pkt(2, sign(P, 0x28, data, created=S.created + 100))
    return out

def armor(kind, data, crc=True, headers=b""):
    b = base64.b64encode(data)
    lines = b"\n".join(b[i:i + 64] for i in range(0, len(b), 64))
    c = 0xB704CE
    for x in data:
        c ^= x << 16
        for _ in range(8):
            c <<= 1
            if c & 0x1000000: c ^= 0x1864CFB
    c &= 0xFFFFFF
    tail = (b"\n=" + base64.b64encode(struct.pack(">I", c)[1:])) if crc else b""
    return b"-----BEGIN " + kind + b"-----\n" + headers + b"\n" + lines + tail + b"\n-----END " + kind + b"-----\n"

def canon(text):
    lines = text.split(b"\n")
    if lines and lines[-1] == b"": lines = lines[:-1]
    return b"\r\n".join(l.rstrip(b" \t") for l in lines)

def clearsign(text, sigpkts, hashhdr=b"SHA256"):
    body = b"".join((b"- " + l if l.startswith(b"-") else l) + b"\n" for l in text.split(b"\n")[:-1])
    hdr = b"-----BEGIN PGP SIGNED MESSAGE-----\n" + (b"Hash: " + hashhdr + b"\n" if hashhdr else b"") + b"\n"
    return hdr + body + armor(b"PGP SIGNATURE", b"".join(pkt(2, s) for s in sigpkts))

def write(name, data):
    p = os.path.join(D, name)
    with open(p, "wb") as f: f.write(data)
    return p

def run(args):
    r = subprocess.run([tool] + args, capture_output=True, text=True, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1"))
    return r.returncode, r.stdout + r.stderr

def expect(name, signed, keyrings, code, *statuses, t=NOW, sel=None, detached=None, gpg_ok=None):
    f = write(name + (".txt" if detached is not None else ".asc"), signed)
    krs = [write(f"{name}.kr{i}", k) for i, k in enumerate(keyrings)]
    a = ["-t", str(t)] + (["-s", sel] if sel else [])
    if detached is not None:
        sf = write(name + ".sig", detached)
        rc, o = run(["detached"] + a + [f, sf] + krs)
    else:
        rc, o = run(["clear"] + a + [f] + krs)
    ok = rc == code and all(s in o for s in statuses) and "ERROR: AddressSanitizer" not in o and "runtime error" not in o
    check(ok, f"{name}: want exit {code} and {statuses}, got exit {rc}: {o.strip()[:300]}")
    if gpgv and (gpg_ok if gpg_ok is not None else code == 0):
        # gpgv cannot fix its clock to NOW for us; only cases valid at the real time are compared
        g = subprocess.run([gpgv, "--keyring", krs[0]] + ([sf, f] if detached is not None else [f]), capture_output=True, text=True)
        check(g.returncode == 0, f"{name}: gpgv refuses a file this script made to be valid: {g.stderr.strip()[:300]}")
    return o

TEXT = b"Origin: Test\nLabel: Test\nSuite: test\nSHA256:\n 0123 12 main/binary-amd64/Packages\n"

# ---------------- keys ----------------
R = Key("rsa", bits=3072)
E = Key("ed")
N = Key("ed25519")
R4 = Key("rsa", bits=4096)
OTHER = Key("ed")
REAL = int(__import__("time").time())

def data_sig(k, halgo=8, **kw): return sign(k, 0x01, canon(TEXT), halgo=halgo, **kw)

# ---- algorithms and hashes
for nm, k in (("rsa3072", R), ("rsa4096", R4), ("eddsa-legacy", E), ("ed25519-native", N)):
    for h in (8, 9, 10):
        hn = {8: b"SHA256", 9: b"SHA384", 10: b"SHA512"}[h]
        expect(f"{nm}-h{h}", clearsign(TEXT, [data_sig(k, h)], hn), [tpk(k)], 0, "GOODSIG", t=REAL,
               gpg_ok=(k is not N))   # GnuPG 2.4 has no v4 native Ed25519 (algorithm 27)
expect("sha1-data", clearsign(TEXT, [data_sig(R, 2)], b"SHA1"), [tpk(R)], 1, "UNSUPPORTED", "SHA-1")
expect("md5-data", clearsign(TEXT, [data_sig(R, 1)], b"MD5"), [tpk(R)], 1, "UNSUPPORTED", "MD5")
expect("no-hash-header", clearsign(TEXT, [data_sig(E)], None), [tpk(E)], 1, "BADSIG", "Hash header")  # gpgv: "signature digest conflict"
expect("hash-header-mismatch", clearsign(TEXT, [data_sig(E, 8)], b"SHA512"), [tpk(E)], 1, "BADSIG", "Hash header")
expect("hash-header-list", clearsign(TEXT, [data_sig(E, 8)], b"SHA512, SHA256"), [tpk(E)], 0, "GOODSIG")
expect("rsa1024", clearsign(TEXT, [data_sig(Key("rsa", bits=1024))], b"SHA256"), [], 1, "NO_PUBKEY")
K1 = Key("rsa", bits=1024)
expect("rsa1024-in-keyring", clearsign(TEXT, [data_sig(K1)]), [tpk(K1)], 1, "UNUSABLE", "2048")
expect("wrong-left16", clearsign(TEXT, [data_sig(E, left16=b"\0\0")]), [tpk(E)], 1, "BADSIG")

# ---- signatures: math, time, subpackets
expect("corrupt-sig-rsa", clearsign(TEXT, [data_sig(R, corrupt=True)]), [tpk(R)], 1, "BADSIG")
expect("corrupt-sig-ed", clearsign(TEXT, [data_sig(E, corrupt=True)]), [tpk(E)], 1, "BADSIG")
expect("unknown-key", clearsign(TEXT, [data_sig(OTHER)]), [tpk(E)], 1, "NO_PUBKEY")
expect("good-plus-unknown", clearsign(TEXT, [data_sig(E), data_sig(OTHER)]), [tpk(E)], 0, "GOODSIG", "NO_PUBKEY", gpg_ok=False)  # gpgv exits 2 on any missing key; apt reads its status lines instead
expect("good-plus-bad", clearsign(TEXT, [data_sig(E), data_sig(R, corrupt=True)]), [tpk(E) + tpk(R)], 1, "GOODSIG", "BADSIG")
expect("bad-plus-good", clearsign(TEXT, [data_sig(R, corrupt=True), data_sig(E)]), [tpk(E) + tpk(R)], 1, "BADSIG")
expect("two-good", clearsign(TEXT, [data_sig(E), data_sig(R)]), [tpk(E) + tpk(R)], 0, "(2 good, 0 bad)")
expect("crit-unknown-subpacket", clearsign(TEXT, [data_sig(E, hashed=sp(100, b"x", crit=True))]), [tpk(E)], 1, "BADSIG", "critical")
expect("noncrit-unknown-subpacket", clearsign(TEXT, [data_sig(E, hashed=sp(100, b"x"))]), [tpk(E)], 0, "GOODSIG")
expect("crit-notation", clearsign(TEXT, [data_sig(E, hashed=sp(20, b"\x80\0\0\0\0\x01\0\x01ax", crit=True))]), [tpk(E)], 1, "BADSIG")
expect("crit-unknown-unhashed", clearsign(TEXT, [data_sig(E, unhashed=sp(101, b"x", crit=True))]), [tpk(E)], 1, "BADSIG")
expect("no-creation-time", clearsign(TEXT, [data_sig(E, issuer=False, hashed=b"", unhashed=sp(16, E.keyid))]), [tpk(E)], 1, "BADSIG", "creation")
expect("creation-only-unhashed", clearsign(TEXT, [data_sig(E, issuer=False, hashed=b"", unhashed=sp(16, E.keyid) + sp(2, struct.pack(">I", T0 + 10)))]), [tpk(E)], 1, "BADSIG")
expect("issuer-keyid-only", clearsign(TEXT, [data_sig(E, issuer="nofpr")]), [tpk(E)], 0, "GOODSIG")
expect("no-issuer-at-all", clearsign(TEXT, [data_sig(E, issuer=False, hashed=sp(2, struct.pack(">I", T0 + 10)))]), [tpk(E)], 0, "GOODSIG", gpg_ok=False)
expect("expired-sig", clearsign(TEXT, [data_sig(E, hashed=sp(3, struct.pack(">I", 3600)))]), [tpk(E)], 1, "EXPSIG")
expect("expired-sig-early-clock", clearsign(TEXT, [data_sig(E, hashed=sp(3, struct.pack(">I", 3600)))]), [tpk(E)], 0, "GOODSIG", t=T0 + 20, gpg_ok=False)
expect("sig-before-key", clearsign(TEXT, [data_sig(E, created=T0 - 100)]), [tpk(E)], 1, "BADSIG", "older than the key")
expect("sig-in-future", clearsign(TEXT, [data_sig(E, created=NOW + 1000)]), [tpk(E)], 1, "UNUSABLE", "clock")
expect("v3-sig", clearsign(TEXT, [bytes([3, 5, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 8, 0, 0])]), [tpk(E)], 1, "UNSUPPORTED", "version 3")
expect("v6-sig", clearsign(TEXT, [bytes([6, 1, 27, 10, 0, 0, 0, 0]) + bytes(64)]), [tpk(E)], 1, "UNSUPPORTED", "version 6")
expect("v5-sig", clearsign(TEXT, [bytes([5, 1, 22, 8, 0, 0, 0, 0])]), [tpk(E)], 1, "UNSUPPORTED", "version 5")
expect("binary-type-in-cleartext", clearsign(TEXT, [sign(E, 0x00, canon(TEXT))]), [tpk(E)], 1, "UNSUPPORTED", "0x00")
expect("fpr-mismatch", clearsign(TEXT, [data_sig(E, issuer=False, hashed=sp(2, struct.pack(">I", T0 + 10)) + sp(33, b"\x04" + OTHER.fpr), unhashed=sp(16, E.keyid))]), [tpk(E)], 1, "NO_PUBKEY")
expect("algo-mismatch", clearsign(TEXT, [data_sig(E, pkalgo=1)]), [tpk(E)], 2, "MALFORMED")

# ---- keys: self-signatures, flags, expiry, revocation
expect("self-sha1", clearsign(TEXT, [data_sig(R)]), [tpk(R, selfhash=2)], 1, "UNUSABLE", "SHA-1")
expect("self-corrupt", clearsign(TEXT, [data_sig(R)]), [tpk(R, selfcorrupt=True)], 1, "UNUSABLE", "no valid self-signature")
expect("self-crit-unknown", clearsign(TEXT, [data_sig(E)]), [tpk(E, selfextra=sp(99, b"z", crit=True))], 1, "UNUSABLE")
expect("certify-only-primary", clearsign(TEXT, [data_sig(E)]), [tpk(E, flags=b"\x01")], 1, "UNUSABLE", "flags")
expect("no-flags-primary", clearsign(TEXT, [data_sig(E)]), [tpk(E, flags=None)], 0, "GOODSIG")
expect("key-expired", clearsign(TEXT, [data_sig(E)]), [tpk(E, keyexp=86400)], 1, "EXPKEYSIG")
expect("key-not-yet-expired", clearsign(TEXT, [data_sig(E)]), [tpk(E, keyexp=86400)], 0, "GOODSIG", t=T0 + 3600, gpg_ok=False)
expect("key-revoked", clearsign(TEXT, [data_sig(E)]), [tpk(E, revoke=True)], 1, "REVKEYSIG")
expect("revocation-forged", clearsign(TEXT, [data_sig(E)]), [tpk(E, revoke_bad=True)], 0, "GOODSIG")
expect("uid-revoked", clearsign(TEXT, [data_sig(E)]), [tpk(E, uid_revoked=True)], 1, "UNUSABLE")
S1 = Key("rsa", bits=3072, created=T0 + 5)
S2 = Key("ed", created=T0 + 5)
expect("subkey-good", clearsign(TEXT, [data_sig(S1)]), [tpk(R, subs=[subkey(R, S1)])], 0, "GOODSIG", t=REAL)
expect("subkey-ed-good", clearsign(TEXT, [data_sig(S2)]), [tpk(R, subs=[subkey(R, S2)])], 0, "GOODSIG", t=REAL)
expect("subkey-no-backsig", clearsign(TEXT, [data_sig(S1)]), [tpk(R, subs=[subkey(R, S1, backsig=False)])], 1, "UNUSABLE", "back-signature")
expect("subkey-backsig-by-primary", clearsign(TEXT, [data_sig(S1)]), [tpk(R, subs=[subkey(R, S1, backsig_by=R)])], 1, "UNUSABLE", "back-signature")
expect("subkey-backsig-by-other", clearsign(TEXT, [data_sig(S1)]), [tpk(R, subs=[subkey(R, S1, backsig_by=S2)])], 1, "UNUSABLE")
expect("subkey-no-sign-flag", clearsign(TEXT, [data_sig(S1)]), [tpk(R, subs=[subkey(R, S1, flags=b"\x0c")])], 1, "UNUSABLE")
expect("subkey-no-flags", clearsign(TEXT, [data_sig(S1)]), [tpk(R, subs=[subkey(R, S1, flags=None)])], 1, "UNUSABLE")
expect("subkey-binding-forged", clearsign(TEXT, [data_sig(S1)]), [tpk(R, subs=[subkey(R, S1, bindcorrupt=True)])], 1, "UNUSABLE", "binding")
expect("subkey-binding-sha1", clearsign(TEXT, [data_sig(S1)]), [tpk(R, subs=[subkey(R, S1, bindhash=2)])], 1, "UNUSABLE", "SHA-1")
expect("subkey-revoked", clearsign(TEXT, [data_sig(S1)]), [tpk(R, subs=[subkey(R, S1, revoke=True)])], 1, "REVKEYSIG")
expect("subkey-expired", clearsign(TEXT, [data_sig(S1)]), [tpk(R, subs=[subkey(R, S1, keyexp=86400)])], 1, "EXPKEYSIG")
expect("subkey-primary-expired", clearsign(TEXT, [data_sig(S1)]), [tpk(R, keyexp=86400, subs=[subkey(R, S1)])], 1, "EXPKEYSIG")
expect("subkey-primary-revoked", clearsign(TEXT, [data_sig(S1)]), [tpk(R, revoke=True, subs=[subkey(R, S1)])], 1, "REVKEYSIG")
expect("subkey-unbound-alone", clearsign(TEXT, [data_sig(S1)]), [tpk(R) + S1.packet(sub=True)], 1, "UNUSABLE")
expect("select-primary-keeps-subkey", clearsign(TEXT, [data_sig(S1)]), [tpk(R, subs=[subkey(R, S1)])], 0, "GOODSIG", sel=R.fpr.hex())
expect("select-exact-primary-drops-subkey", clearsign(TEXT, [data_sig(S1)]), [tpk(R, subs=[subkey(R, S1)])], 1, "NO_PUBKEY", sel=R.fpr.hex() + "!")
expect("select-exact-subkey", clearsign(TEXT, [data_sig(S1)]), [tpk(R, subs=[subkey(R, S1)])], 0, "GOODSIG", sel=S1.fpr.hex().upper() + "!")
expect("select-other", clearsign(TEXT, [data_sig(E)]), [tpk(E) + tpk(R)], 1, "NO_PUBKEY", sel=R.fpr.hex())
expect("small-order-key", clearsign(TEXT, [data_sig(E)]), [tpk(E) + pkt(6, bytes([4]) + struct.pack(">I", T0) + bytes([22, 9]) + bytes.fromhex("2b06010401da470f01") + mpi(b"\x40" + bytes(32)))], 0, "GOODSIG")
o = run(["keys", write("small.kr", pkt(6, bytes([4]) + struct.pack(">I", T0) + bytes([22, 9]) + bytes.fromhex("2b06010401da470f01") + mpi(b"\x40" + bytes([1]) + bytes(31))))])[1]
check("small order" in o, f"a small-order Ed25519 key is flagged: {o.strip()}")
o = run(["keys", write("v6.kr", pkt(6, bytes([6]) + struct.pack(">I", T0) + bytes([27]) + struct.pack(">I", 32) + bytes(32)))])[1]
check("version 6 keys are not supported" in o, f"a version 6 key is listed as unsupported: {o.strip()}")
o = run(["keys", write("old-format.kr", pkt(6, E.body, old=True) + pkt(13, b"x", old=True) + pkt(2, sign(E, 0x13, E.hashed_key() + uidpart(b"x"), hashed=sp(27, b"\x03")), old=True))])[1]
check("can_sign=1" in o, f"old-format packets are read: {o.strip()}")
o = run(["keys", write("secret.kr", pkt(5, E.body + b"\0" + bytes(34)) + pkt(13, b"x") + tpk(R))])[1]
check(o.count("pub ") == 1 and R.fpr.hex().upper() in o, f"a secret key in a keyring is skipped, the next key read: {o.strip()}")
rc, o = run(["keys", write("kbx.kr", b"\0\0\0\x20\x01\x01\0\0KBXf" + bytes(20))])
check(rc == 2 and "keybox" in o, f"a GnuPG keybox is refused with a clear message: {o.strip()}")
rc, o = run(["keys", write("partial.kr", bytes([0xc6, 0xe1]) + bytes(2))])
check(rc == 2 and "partial" in o, f"a partial body length in a key packet is refused: {o.strip()}")
rc, o = run(["keys", write("indeterminate.kr", bytes([0x9b]) + E.body)])
check(rc == 2 and "indeterminate" in o, f"the old format's indeterminate length is refused: {o.strip()}")
rc, o = run(["keys", write("truncated.kr", tpk(E)[:-5])])
check(rc == 2, f"a truncated keyring is refused: {o.strip()}")
rc, o = run(["keys", write("armored.kr", armor(b"PGP PUBLIC KEY BLOCK", tpk(E), headers=b"Comment: test\n") + b"\nsome text\n" + armor(b"PGP PUBLIC KEY BLOCK", tpk(R), crc=False))])
check(rc == 0 and o.count("pub ") == 2, f"two armored key blocks in one file, with and without CRC: {o.strip()}")

# ---- detached signatures
DOC = b"Origin: Test\nSuite: test\n  trailing spaces stay  \r\nbinary \x00 bytes\n"
dsig = pkt(2, sign(E, 0x00, DOC))
expect("detached-binary", DOC, [tpk(E)], 0, "GOODSIG", detached=dsig, t=REAL)
expect("detached-armored", DOC, [tpk(E)], 0, "GOODSIG", detached=armor(b"PGP SIGNATURE", dsig), t=REAL)
expect("detached-changed", DOC + b"x", [tpk(E)], 1, "BADSIG", detached=dsig)
expect("detached-text-type", DOC, [tpk(E)], 1, "UNSUPPORTED", detached=pkt(2, sign(E, 0x01, DOC)))
expect("detached-trailing-text", DOC, [tpk(E)], 2, "MALFORMED", detached=armor(b"PGP SIGNATURE", dsig) + b"junk\n")
expect("detached-literal-packet", DOC, [tpk(E)], 2, "MALFORMED", detached=pkt(11, b"b\0\0\0\0\0data") + dsig)
expect("detached-empty", DOC, [tpk(E)], 2, "MALFORMED", detached=b"")
expect("detached-marker-ok", DOC, [tpk(E)], 0, "GOODSIG", detached=pkt(10, b"PGP") + dsig)

# ---- cleartext framing and the classic attacks
good = clearsign(TEXT, [data_sig(E)])
expect("clear-good", good, [tpk(E)], 0, "GOODSIG", t=REAL)
rc, o = run(["text", write("clear-good-text.asc", good)])
check(rc == 0 and o.encode() == TEXT, f"the signed text is returned exactly: {o!r}")
expect("text-before-header", b"Origin: Evil\n" + good, [tpk(E)], 2, "MALFORMED")
expect("blank-before-header", b"\n" + good, [tpk(E)], 2, "MALFORMED")
expect("text-after-signature", good + b"Origin: Evil\n", [tpk(E)], 2, "MALFORMED", "after the signature")
expect("blank-after-signature", good + b"\n\n", [tpk(E)], 0, "GOODSIG")
expect("two-cleartext-blocks", good + clearsign(b"Origin: Evil\n", [data_sig(E)]), [tpk(E)], 2, "MALFORMED")
expect("second-sig-block", good + armor(b"PGP SIGNATURE", pkt(2, data_sig(E))), [tpk(E)], 2, "MALFORMED")
fake = b"-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA512\n\n" + TEXT + b"-----BEGIN PGP SIGNATURE-----\n\nnot a real signature\n-----END PGP SIGNATURE-----\n"
expect("looks-signed", fake, [tpk(E)], 2, "MALFORMED")
expect("no-signature-block", b"-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA256\n\n" + TEXT, [tpk(E)], 2, "MALFORMED")
expect("plain-release", TEXT, [tpk(E)], 2, "MALFORMED")
expect("nul-byte", good.replace(b"Label: Test", b"Label: T\0st"), [tpk(E)], 2, "MALFORMED", "NUL")
long_text = TEXT + b"X: " + b"a" * 20000 + b"\n"
expect("long-line", clearsign(long_text, [sign(E, 0x01, canon(long_text))]), [tpk(E)], 2, "MALFORMED", "too long")
expect("extra-armor-header", good.replace(b"Hash: SHA256\n", b"Hash: SHA256\nComment: hi\n"), [tpk(E)], 2, "MALFORMED")
expect("missing-blank-line", good.replace(b"Hash: SHA256\n\n", b"Hash: SHA256\n"), [tpk(E)], 2, "MALFORMED")
expect("unknown-hash-name", good.replace(b"Hash: SHA256", b"Hash: SHA256, WHIRLPOOL"), [tpk(E)], 2, "MALFORMED")
nested = TEXT + b"-----BEGIN PGP SIGNED MESSAGE-----\n"
expect("unescaped-dash-line", good.replace(b"Origin: Test\n", b"Origin: Test\n-----BEGIN PGP SIGNED MESSAGE-----\n"), [tpk(E)], 2, "MALFORMED", "dash")
dashed = b"Origin: Test\n-----BEGIN PGP SIGNATURE-----\n- x\n"
expect("dash-escaped-content", clearsign(dashed, [sign(E, 0x01, canon(dashed))]), [tpk(E)], 0, "GOODSIG")
rc, o = run(["text", write("dash-text.asc", clearsign(dashed, [sign(E, 0x01, canon(dashed))]))])
check(o.encode() == dashed, f"dash escapes are removed in the returned text: {o!r}")
expect("trailing-ws-added", good.replace(b"Label: Test\n", b"Label: Test  \t\n"), [tpk(E)], 0, "GOODSIG")
rc, o = run(["text", write("trailing-text.asc", good.replace(b"Label: Test\n", b"Label: Test  \t\n"))])
check(o.encode() == TEXT, f"trailing white space (not signed) is not returned: {o!r}")
expect("crlf-file", good.replace(b"\n", b"\r\n"), [tpk(E)], 0, "GOODSIG")
expect("bare-cr", good.replace(b"Label: Test", b"Label: Te\rst"), [tpk(E)], 2, "MALFORMED")
expect("body-changed", good.replace(b"Label: Test", b"Label: Tesu"), [tpk(E)], 1, "BADSIG")
expect("body-line-added", good.replace(b"Label: Test\n", b"Label: Test\nX: y\n"), [tpk(E)], 1, "BADSIG")
expect("body-empty-line-added", good.replace(b"Label: Test\n", b"Label: Test\n\n"), [tpk(E)], 1, "BADSIG")
sigb64 = good[good.index(b"-----BEGIN PGP SIGNATURE-----"):]
lines = sigb64.split(b"\n")
crcline = [l for l in lines if l.startswith(b"=")][0]
expect("crc-wrong", good.replace(crcline, b"=" + (b"A" if crcline[1:2] != b"A" else b"B") + crcline[2:]), [tpk(E)], 2, "MALFORMED", "CRC")
expect("crc-missing", good.replace(crcline + b"\n", b""), [tpk(E)], 0, "GOODSIG")
expect("armor-version-header", good.replace(b"-----BEGIN PGP SIGNATURE-----\n", b"-----BEGIN PGP SIGNATURE-----\nVersion: x\n"), [tpk(E)], 0, "GOODSIG")
expect("armor-bad-char", good.replace(lines[2], lines[2][:10] + b"*" + lines[2][11:]), [tpk(E)], 2, "MALFORMED", "base64")
expect("armor-end-mismatch", good.replace(b"-----END PGP SIGNATURE-----", b"-----END PGP MESSAGE-----"), [tpk(E)], 2, "MALFORMED")
expect("sig-removed", good[:good.index(b"-----BEGIN PGP SIGNATURE-----")] + armor(b"PGP SIGNATURE", b""), [tpk(E)], 2, "MALFORMED")
expect("sig-literal", good[:good.index(b"-----BEGIN PGP SIGNATURE-----")] + armor(b"PGP SIGNATURE", pkt(11, b"b\0\0\0\0\0x")), [tpk(E)], 2, "MALFORMED")
expect("sig-partial-length", good[:good.index(b"-----BEGIN PGP SIGNATURE-----")] + armor(b"PGP SIGNATURE", bytes([0xc2, 0xe0]) + b"\x04"), [tpk(E)], 2, "MALFORMED", "partial")
expect("sig-truncated", good[:good.index(b"-----BEGIN PGP SIGNATURE-----")] + armor(b"PGP SIGNATURE", pkt(2, data_sig(E))[:-3]), [tpk(E)], 2, "MALFORMED")
expect("sig-trailing-bytes", good[:good.index(b"-----BEGIN PGP SIGNATURE-----")] + armor(b"PGP SIGNATURE", pkt(2, data_sig(E) + b"\0")), [tpk(E)], 2, "MALFORMED")
expect("many-signatures", clearsign(TEXT, [data_sig(E)] * 40), [tpk(E)], 2, "MALFORMED", "too many")
expect("no-text-lines", clearsign(b"", [sign(E, 0x01, b"")]), [tpk(E)], 0, "GOODSIG")
expect("no-final-newline-before-sig", good.replace(b"Packages\n-----BEGIN PGP SIGNATURE", b"Packages-----BEGIN PGP SIGNATURE"), [tpk(E)], 2, "MALFORMED")

print(f"pgpcases: {checks} checks, {fails} failed")
sys.exit(1 if fails else 0)
