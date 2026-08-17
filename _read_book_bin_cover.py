"""Best-effort peek at cover href strings in book.bin"""
import os, re

for d in ["epub_1866614315", "epub_3660743696", "epub_2980191668", "epub_597965850"]:
    path = os.path.join(r"H:\.crosspoint", d, "book.bin")
    if not os.path.exists(path):
        print(d, "no book.bin")
        continue
    data = open(path, "rb").read()
    # printable strings that look like image paths
    strings = re.findall(rb"[\x20-\x7e]{4,120}", data)
    hits = [s.decode() for s in strings if re.search(r"cover|\.jpe?g|\.png|images/", s.decode(), re.I)]
    print(d, "size", len(data), "hits", hits[:20])
