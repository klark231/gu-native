#!/usr/bin/env python3
"""Builds runtime/settings_locale_zh.c from tools/language_zh.tsv and tools/language_zh_extra.tsv.

Each line is English<TAB>Chinese (UTF-8); # or ; start a comment; \\n, \\t and \\\\ are escapes. The extra file wins
where both have a string. The C file holds them sorted by their English bytes, for a binary search (settings_text).
Run it again after editing either file:  python tools/gen_locale_zh.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SOURCES = [os.path.join(HERE, "language_zh.tsv"), os.path.join(HERE, "language_zh_extra.tsv")]
OUT = os.path.join(ROOT, "runtime", "settings_locale_zh.c")


def unescape(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s) and s[i + 1] in "nt\\":
            out.append({"n": "\n", "t": "\t", "\\": "\\"}[s[i + 1]])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def c_string(s):
    # (UTF-8 as it is, as runtime/settings_locale.h has its Spanish)
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t") + '"'


def main():
    table = {}
    for path in SOURCES:
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                line = line.rstrip("\r\n")
                if not line or line[0] in "#;":
                    continue
                if "\t" not in line:
                    sys.exit("%s:%d: no tab between the English and the Chinese" % (path, n))
                en, zh = line.split("\t", 1)
                table[unescape(en)] = unescape(zh)
    keys = sorted(table, key=lambda k: k.encode("utf-8"))
    with open(OUT, "w", encoding="utf-8", newline="\n") as o:
        o.write("// Made by tools/gen_locale_zh.py from tools/language_zh*.tsv: edit those and run it again, not this file.\n")
        o.write("// The settings menu's Chinese (Simplified), by the English text, sorted for a binary search.\n")
        o.write("#include <stddef.h>\n#include <string.h>\n\n")
        o.write("static const struct { const char *en, *zh; } ZH[] = {\n")
        for k in keys:
            o.write("    {%s, %s},\n" % (c_string(k), c_string(table[k])))
        o.write("};\n\n")
        o.write("volatile int settings_zh_used;  // (set once Chinese is shown: the overlay then loads a font with its glyphs)\n\n")
        o.write("const char *settings_text_zh(const char *english) {\n")
        o.write("    settings_zh_used = 1;\n")
        o.write("    size_t lo = 0, hi = sizeof ZH / sizeof *ZH;\n")
        o.write("    while (lo < hi) {\n")
        o.write("        size_t mid = (lo + hi) / 2;\n")
        o.write("        int c = strcmp(english, ZH[mid].en);\n")
        o.write("        if (!c) return ZH[mid].zh;\n")
        o.write("        if (c < 0) hi = mid;\n")
        o.write("        else lo = mid + 1;\n")
        o.write("    }\n")
        o.write("    return english;  // (not translated yet: English)\n")
        o.write("}\n")
    print("%s: %d strings" % (os.path.relpath(OUT, ROOT), len(keys)))


if __name__ == "__main__":
    main()
