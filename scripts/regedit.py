#!/usr/bin/env python3
"""Set a value in a Wine registry file (system.reg / user.reg) while wine isn't running.
   regedit.py FILE 'Key\\Path' NAME VALUE
Key path uses single backslashes, relative to the hive (e.g. 'Software\\Wine\\DllOverrides')."""
import re, sys, time

def esc(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')

def main():
    path, key, name, value = sys.argv[1:5]
    s = open(path, encoding='utf-8', errors='surrogateescape').read()
    hdr_text = '[' + key.replace('\\', '\\\\') + ']'
    line = '"%s"="%s"' % (esc(name), esc(value))
    # find the key header (case-insensitive, like Wine)
    m = None
    for mm in re.finditer(r'^\[([^\]]*)\][^\n]*\n', s, re.M):
        if mm.group(1).lower() == hdr_text[1:-1].lower():
            m = mm
            break
    if m:
        end = s.find('\n\n', m.end())
        end = len(s) if end < 0 else end
        block = s[m.end():end]
        block = re.sub(r'^"%s"=.*\n?' % re.escape(esc(name)), '', block, flags=re.M | re.I)
        block = block.rstrip('\n')
        block = (block + '\n' if block else '') + line
        s = s[:m.end()] + block + s[end:]
    else:
        if not s.endswith('\n'):
            s += '\n'
        s += '\n%s %d\n%s\n' % (hdr_text, int(time.time()), line)
    open(path, 'w', encoding='utf-8', errors='surrogateescape').write(s)

main()
