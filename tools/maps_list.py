#!/usr/bin/env python3
"""
maps_list.py: builds maps.json for DOWNLOAD MAPS from the "Map Releases"
discussions on nzp-team/nzportable.

For each release it takes the zip attached on GitHub (the PC one when there
are several), checks it is a zip the PS3 can unpack (stored/deflate, no zip64)
and has a .bsp, and writes name/author/size/bsp/desc. Zips are linked where
their authors posted them, not re-hosted. Zips already checked (same URL) are
taken from the previous maps.json instead of downloaded again.

  GITHUB_TOKEN=... python3 maps_list.py old_maps.json new_maps.json
"""
import io, json, os, re, struct, sys, urllib.request, zipfile

REPO_OWNER, REPO_NAME, CATEGORY = "nzp-team", "nzportable", "Map Releases"
TOKEN = os.environ["GITHUB_TOKEN"]
MAX_ZIP = 300 * 1024 * 1024

ZIP_RE = re.compile(r'https://github\.com/(?:user-attachments/files/\d+|[\w.-]+/[\w.-]+/(?:files/\d+|releases/download/[^/\s]+))/[^\s)"\'<>\]]+?\.zip', re.I)
OTHER_PLATFORMS = re.compile(r'psp|3ds|ctr|vita|psvita|nspire|switch|nx\b|wii', re.I)


def graphql(query, variables):
    req = urllib.request.Request("https://api.github.com/graphql",
        data=json.dumps({"query": query, "variables": variables}).encode(),
        headers={"Authorization": "bearer " + TOKEN, "Content-Type": "application/json"})
    out = json.load(urllib.request.urlopen(req, timeout=60))
    if "errors" in out:
        raise SystemExit(out["errors"])
    return out["data"]


def discussions():
    cats = graphql("""query($o:String!,$n:String!){repository(owner:$o,name:$n){
        discussionCategories(first:50){nodes{id name}}}}""", {"o": REPO_OWNER, "n": REPO_NAME})
    cat = next(c["id"] for c in cats["repository"]["discussionCategories"]["nodes"] if c["name"] == CATEGORY)
    after = None
    while True:
        d = graphql("""query($o:String!,$n:String!,$c:ID!,$a:String){repository(owner:$o,name:$n){
            discussions(first:50,after:$a,categoryId:$c,orderBy:{field:UPDATED_AT,direction:DESC}){
              pageInfo{hasNextPage endCursor}
              nodes{number title url body closed author{login}}}}}""",
            {"o": REPO_OWNER, "n": REPO_NAME, "c": cat, "a": after})["repository"]["discussions"]
        yield from d["nodes"]
        if not d["pageInfo"]["hasNextPage"]:
            return
        after = d["pageInfo"]["endCursor"]


def pick_zip(body):
    urls = list(dict.fromkeys(ZIP_RE.findall(body)))
    if not urls:
        return None
    def rank(u):
        name = u.rsplit("/", 1)[-1]
        if re.search(r'pc|vril|desktop|windows', name, re.I):
            return 0
        if OTHER_PLATFORMS.search(name):
            return 2
        return 1
    urls.sort(key=rank)
    return urls[0] if rank(urls[0]) < 2 else None


def check_zip(url):
    """Downloads the zip; returns (size, [bsp names]) or a reason it can't be used."""
    data = urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "nzp-ps3-maps"}), timeout=300).read(MAX_ZIP + 1)
    if len(data) > MAX_ZIP:
        return "too big"
    try:
        z = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile:
        return "not a zip"
    bsps = []
    for info in z.infolist():
        if info.compress_type not in (0, 8):
            return "compression %d" % info.compress_type
        if info.flag_bits & 1:
            return "encrypted"
        if info.file_size >= 0xFFFFFFFF or info.compress_size >= 0xFFFFFFFF or info.header_offset >= 0xFFFFFFFF:
            return "zip64"
        name = info.filename.replace("\\", "/")
        if name.lower().endswith(".bsp") and "__macosx" not in name.lower():
            bsps.append(name.rsplit("/", 1)[-1][:-4])
    if len(z.infolist()) > 0xFFFF:
        return "zip64"
    if not bsps:
        return "no .bsp"
    return len(data), bsps


def clean_title(t):
    t = re.sub(r'\[[^\]]*\]|\([^)]*(update|v\d)[^)]*\)', '', t, flags=re.I)
    return re.sub(r'\s+', ' ', t).strip(" -!") or t.strip()


def version_of(title, body):
    m = re.search(r'\bv(?:ersion)?\s*(\d+(?:\.\d+)*)', title, re.I) or re.search(r'update\s*(\d+(?:\.\d+)*)', title, re.I)
    return m.group(1) if m else ""


def desc_of(body):
    text = re.sub(r'<[^>]+>|!\[[^\]]*\]\([^)]*\)|\[([^\]]*)\]\([^)]*\)', r'\1', body)
    text = re.sub(r'https?://\S+|[#*_`>|]', '', text)
    words, lines, cur = text.split(), [], ""
    for w in words:
        if len(cur) + len(w) + 1 > 46:
            lines.append(cur)
            cur = w
            if len(lines) == 4:
                break
        else:
            cur = (cur + " " + w).strip()
    if cur and len(lines) < 4:
        lines.append(cur)
    return lines


def main():
    old = {}
    if len(sys.argv) > 2 and os.path.exists(sys.argv[1]):
        try:
            for m in json.load(open(sys.argv[1])).get("maps", []) + json.load(open(sys.argv[1])).get("skipped", []):
                old[m["file"]] = m
        except Exception:
            pass
    maps, skipped, seen_bsp = [], [], set()
    for d in discussions():
        url = pick_zip(d["body"] or "")
        if not url:
            continue
        prev = old.get(url)
        if prev and "reason" in prev:
            skipped.append(prev)
            continue
        if prev:
            size, bsps = prev["size"], prev.get("bsps", [prev["bsp"]])
        else:
            try:
                r = check_zip(url)
            except Exception as e:
                print("  fetch failed:", url, e)
                continue
            if isinstance(r, str):
                print("  skip #%d %s: %s" % (d["number"], d["title"], r))
                skipped.append({"file": url, "reason": r, "thread": d["url"]})
                continue
            size, bsps = r
        bsp = bsps[0]
        if bsp.lower() in seen_bsp:
            continue
        seen_bsp.add(bsp.lower())
        maps.append({
            "name": clean_title(d["title"])[:60],
            "author": (d["author"] or {}).get("login", ""),
            "version": version_of(d["title"], d["body"]),
            "file": url,
            "size": size,
            "bsp": bsp,
            "bsps": bsps,
            "desc": desc_of(d["body"] or ""),
            "thread": d["url"],
        })
        print("  ok #%d %s -> %s (%d bytes)" % (d["number"], d["title"], bsp, size))
    maps.sort(key=lambda m: m["name"].lower())
    json.dump({"source": "https://github.com/%s/%s/discussions" % (REPO_OWNER, REPO_NAME),
               "maps": maps, "skipped": skipped}, open(sys.argv[-1], "w"), indent=1)
    print("%d maps, %d skipped" % (len(maps), len(skipped)))


if __name__ == "__main__":
    main()
