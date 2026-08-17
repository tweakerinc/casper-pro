import re
p=r"C:\Users\m\Documents\CasperPro\.pio\build\x4pro\firmware.bin"
d=open(p,"rb").read()
for s in [b"GT911", b"X4PRO", b"touch", b"Penumbra", b"Casper", b"v0.1.9-pro", b"FREEINK"]:
    print(s, d.find(s))
# find version string
for m in re.finditer(rb"v0\.1\.9[^\x00]{0,20}", d):
    print("ver", m.group())
