import hashlib, struct, os
dev=open(r"C:\Users\m\Documents\CasperPro\_on_device_64k.bin","rb").read()
bld=open(r"C:\Users\m\Documents\CasperPro\.pio\build\x4pro\firmware.bin","rb").read(65536)
print("match64k", hashlib.sha256(dev).hexdigest()==hashlib.sha256(bld).hexdigest())
print("devsha", hashlib.sha256(dev).hexdigest()[:24])
print("bldsha", hashlib.sha256(bld).hexdigest()[:24])
# Parse partition table (ESP format at 0x8000)
pt=open(r"C:\Users\m\Documents\CasperPro\_part.bin","rb").read()
print("part magic", pt[:2].hex())
# entries 32 bytes each, magic 0x50AA
off=0
# skip if md5 at start? Standard is magic AA 50
while off+32 <= len(pt):
  e=pt[off:off+32]
  if e[0]==0xAA and e[1]==0x50:
    typ=e[2]; subt=e[3]
    offset=struct.unpack_from("<I", e, 4)[0]
    size=struct.unpack_from("<I", e, 8)[0]
    label=e[12:28].split(b"\x00")[0].decode("ascii","replace")
    flags=struct.unpack_from("<I", e, 28)[0]
    print(f"  {label:16} type={typ} subt={subt} offset=0x{offset:x} size=0x{size:x} flags={flags}")
    off+=32
  elif e==b"\xff"*32:
    break
  else:
    # try find next
    if off==0 and pt[0:2]!=b"\xaa\x50":
      # sometimes padded
      pass
    off+=32
    if off>0xC00: break
