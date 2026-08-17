import struct, hashlib, os
ota=open(r"C:\Users\m\Documents\CasperPro\_otadata.bin","rb").read()
# ESP OTA data: two slots of 32 bytes typically, or 0x20 each in 0x2000 region
# Format: seq (u32), crc, etc. See esp_ota_ops
print("otadata size", len(ota))
print("otadata hex first 64", ota[:64].hex())
print("otadata hex second slot", ota[0x20:0x40].hex() if len(ota)>=0x40 else "n/a")
# Also check for 0x1000-separated dual copy
print("otadata @0x1000", ota[0x1000:0x1040].hex() if len(ota)>=0x1040 else "n/a")
app1=open(r"C:\Users\m\Documents\CasperPro\_app1_head.bin","rb").read()
print("app1 magic", hex(app1[0]), "first16", app1[:16].hex())
# Decode OTA seq if standard
# struct: uint32 seq, uint8 label[20?], ... actually esp_ota_select_entry_t:
# uint32_t ota_seq; uint8_t seq_label[20]; uint32_t ota_state; uint32_t crc;
for i,name in [(0,"slot0"),(0x20,"slot1"),(0x1000,"copy0"),(0x1020,"copy1")]:
  if i+32 <= len(ota):
    seq=struct.unpack_from("<I", ota, i)[0]
    state=struct.unpack_from("<I", ota, i+24)[0] if i+28<=len(ota) else None
    print(f"{name}: seq={seq} (0x{seq:x}) state={state}")
