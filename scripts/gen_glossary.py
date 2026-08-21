#!/usr/bin/env python3
"""
gen_glossary.py - regenerate the Sunrise codebase glossary from the repo's own
doc-comments. Produces Sunrise-Glossary.html (searchable) and Sunrise-Glossary.md.

Usage (from anywhere):
    python gen_glossary.py <path-to-repo>/Sunrise/src
    # or, if run from inside the src folder, just:
    python gen_glossary.py

No dependencies beyond the Python standard library.
"""
import os, re, sys, json, html

SRC = sys.argv[1] if len(sys.argv) > 1 else "."
OUT_HTML = "Sunrise-Glossary.html"
OUT_MD = "Sunrise-Glossary.md"

MODULE_INTRO = {
    "client": "Reaches INTO the running game — hooks functions, scans memory, moves the player, draws the overlay.",
    "server": "PRETENDS to be Bungie's servers — answers the game's requests so it runs offline.",
    "state": "The DATA the fake server serves — account, activities, gear, unlocks, matchmaking.",
    "middleware": "PLUMBING between client and server — crypto, compression, protobuf, content-package readers.",
    "core": "Shared INFRASTRUCTURE — logging, settings, filesystem, ImGui overlay framework.",
    "steam": "Pretends to be Steam — the DLL masquerades as steam_api64.dll.",
}
ORDER = ["client", "server", "state", "middleware", "core", "steam"]


def lead_comment(path):
    """Return the first /** ... */ doc-comment of a file as one clean line."""
    try:
        txt = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""
    m = re.search(r"/\*\*(.*?)\*/", txt, re.S)
    if not m:
        return ""
    c = re.sub(r"\n\s*\*\s?", " ", m.group(1))  # strip leading * on each line
    # Tags follow the prose, so the summary is everything before the first one. Dropping
    # only the tag word left parameter names and their text running into the truncation.
    parts = re.split(r"@\w+\s*", c)
    lead = re.sub(r"\s+", " ", parts[0]).strip()
    if not lead:
        # A comment that is only tags still has to say something, so keep the tag text.
        lead = re.sub(r"\s+", " ", " ".join(parts[1:])).strip()
    return lead[:240]


def collect(src):
    """dir -> [(header_file, purpose)], only headers that carry a doc-comment."""
    dirmap = {}
    for root, _dirs, files in os.walk(src):
        entries = []
        for h in sorted(f for f in files if f.endswith(".h")):
            p = lead_comment(os.path.join(root, h))
            if p:
                entries.append((h, p))
        if entries:
            rel = os.path.relpath(root, src).replace("\\", "/")
            dirmap[rel] = entries
    return dirmap


def group_by_module(dirmap):
    mods = {}
    for d, e in dirmap.items():
        mods.setdefault(d.split("/")[0], {})[d] = e
    return mods


def write_markdown(mods, total, ndirs):
    L = ["# Sunrise Codebase Glossary", "",
         "> Auto-generated from the repository's own doc-comments by `gen_glossary.py`.",
         "> Regenerate after code changes.", "",
         f"*{total} documented files across {ndirs} directories.*", "", "## Contents", ""]
    for t in ORDER:
        if t in mods:
            L.append(f"- [`{t}/`](#{t}) — {MODULE_INTRO[t]}")
    L.append("")
    for t in ORDER:
        if t not in mods:
            continue
        dirs = mods[t]
        nf = sum(len(e) for e in dirs.values())
        L += [f"\n## {t}/\n", f"**{MODULE_INTRO[t]}** ({nf} files, {len(dirs)} dirs)\n"]
        for d in sorted(dirs):
            L += [f"\n### `{d}/`\n", "| File | Purpose |", "|---|---|"]
            for f, p in dirs[d]:
                L.append(f"| `{f}` | {p.replace('|', chr(92) + '|')} |")
    open(OUT_MD, "w", encoding="utf-8").write("\n".join(L) + "\n")


def write_html(mods, total, ndirs):
    parts = []
    for t in ORDER:
        if t not in mods:
            continue
        dirs = mods[t]
        nf = sum(len(e) for e in dirs.values())
        parts.append(f'<section class="mod"><h2>{t}/ <span class="cnt">{nf} files · {len(dirs)} dirs</span></h2>')
        parts.append(f'<p class="intro">{html.escape(MODULE_INTRO[t])}</p>')
        for d in sorted(dirs):
            parts.append(f'<div class="dir"><h3>{html.escape(d)}/</h3>')
            for f, p in dirs[d]:
                parts.append(f'<div class="file"><span class="fn">{html.escape(f)}</span>'
                             f'<span class="desc">{html.escape(p)}</span></div>')
            parts.append("</div>")
        parts.append("</section>")
    body = "\n".join(parts)
    doc = HTML_TEMPLATE.format(total=total, ndirs=ndirs, body=body)
    open(OUT_HTML, "w", encoding="utf-8").write(doc)


HTML_TEMPLATE = '''<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Sunrise Codebase Glossary</title><style>
:root{{--bg:#0f1419;--card:#1a2130;--ink:#e6edf3;--dim:#8b98a5;--acc:#f5a623;--acc2:#5aa9e6;--line:#2a3441;}}
*{{box-sizing:border-box}}body{{margin:0;font:15px/1.55 -apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--ink)}}
header{{position:sticky;top:0;background:#0f1419ee;backdrop-filter:blur(6px);border-bottom:1px solid var(--line);padding:16px 22px;z-index:10}}
h1{{margin:0 0 4px;font-size:20px}}h1 .sub{{color:var(--acc);font-weight:400;font-size:14px}}
.meta{{color:var(--dim);font-size:13px;margin-bottom:10px}}
#q{{width:100%;padding:10px 14px;font-size:15px;background:var(--card);border:1px solid var(--line);border-radius:9px;color:var(--ink)}}
#q:focus{{outline:none;border-color:var(--acc2)}}
main{{max-width:1000px;margin:0 auto;padding:20px 22px 80px}}.mod{{margin:26px 0}}
.mod>h2{{font-size:22px;margin:0 0 2px;border-bottom:2px solid var(--acc);padding-bottom:6px;display:flex;justify-content:space-between;align-items:baseline}}
.cnt{{font-size:12px;color:var(--dim);font-weight:400}}.intro{{color:var(--dim);margin:8px 0 16px;font-size:14px}}
.dir{{margin:14px 0;background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 16px}}
.dir>h3{{margin:0 0 8px;font-size:14px;color:var(--acc2);font-family:ui-monospace,Menlo,monospace}}
.file{{display:grid;grid-template-columns:230px 1fr;gap:14px;padding:5px 0;border-top:1px solid var(--line)}}
.file:first-of-type{{border-top:none}}.fn{{font-family:ui-monospace,Menlo,monospace;font-size:12.5px;word-break:break-all}}
.desc{{color:var(--dim);font-size:13px}}.hidden{{display:none!important}}.nores{{color:var(--dim);text-align:center;padding:40px}}
</style></head><body><header>
<h1>Sunrise Codebase Glossary <span class="sub">what every module does, mined from the source</span></h1>
<div class="meta">{total} documented files · {ndirs} directories · generated from the repo's own doc-comments</div>
<input id="q" placeholder="Search files or descriptions…" autofocus></header>
<main id="main">{body}<div class="nores hidden" id="nores">No modules match your search.</div></main>
<script>
const q=document.getElementById('q'),files=[...document.querySelectorAll('.file')],dirs=[...document.querySelectorAll('.dir')],mods=[...document.querySelectorAll('.mod')],nores=document.getElementById('nores');
q.addEventListener('input',()=>{{const t=q.value.trim().toLowerCase();
files.forEach(f=>f.classList.toggle('hidden',t&&!f.textContent.toLowerCase().includes(t)));
dirs.forEach(d=>d.classList.toggle('hidden',![...d.querySelectorAll('.file')].some(f=>!f.classList.contains('hidden'))));
mods.forEach(m=>m.classList.toggle('hidden',![...m.querySelectorAll('.dir')].some(d=>!d.classList.contains('hidden'))));
nores.classList.toggle('hidden',[...mods].some(m=>!m.classList.contains('hidden')));}});
</script></body></html>'''


def main():
    if not os.path.isdir(SRC):
        sys.exit(f"Not a directory: {SRC}")
    dirmap = collect(SRC)
    if not dirmap:
        sys.exit(f"No documented .h files found under {SRC} — is this the src folder?")
    mods = group_by_module(dirmap)
    total = sum(len(e) for m in mods.values() for e in m.values())
    ndirs = len(dirmap)
    write_markdown(mods, total, ndirs)
    write_html(mods, total, ndirs)
    print(f"Wrote {OUT_HTML} and {OUT_MD}: {total} files across {ndirs} directories.")


if __name__ == "__main__":
    main()
