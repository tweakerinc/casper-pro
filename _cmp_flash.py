import hashlib, os
def head(p,n=256):
  with open(p,"rb") as f: return f.read(n)
def sha(p,n=65536):
  with open(p,"rb") as f: return hashlib.sha256(f.read(n)).hexdigest()[:20]
dev=r"C:\Users\m\Documents\CasperPro\_on_device_app.bin"
bld=r"C:\Users\m\Documents\CasperPro\.pio\build\x4pro\firmware.bin"
print("device exists", os.path.exists(dev), "size", os.path.getsize(dev) if os.path.exists(dev) else None)
print("build exists", os.path.exists(bld), "size", os.path.getsize(bld) if os.path.exists(bld) else None)
if os.path.exists(dev) and os.path.exists(bld):
  dh, bh = head(dev), head(bld)
  print("device magic", hex(dh[0]), "build magic", hex(bh[0]))
  print("device first32", dh[:32].hex())
  print("build  first32", bh[:32].hex())
  print("sha64k device", sha(dev))
  print("sha64k build ", sha(bld))
  print("match64k", sha(dev)==sha(bld))
