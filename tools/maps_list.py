#!/usr/bin/env python3
"""
maps_list.py: builds maps.json for DOWNLOAD MAPS from the "Map Releases"
discussions on nzp-team/nzportable.

For each release it takes the zip attached on GitHub (the PC one when there
are several), checks it is a zip the PS3 can unpack (stored/deflate, no zip64)
and has a .bsp, and writes name/author/size/bsp/desc. Zips are linked where
their authors posted them, not re-hosted. Zips already checked (same URL) are
taken from the previous maps.json instead of downloaded again.

Each map also gets a picture: the post's first image, cut to 256x144 and saved
as thumbs/<key>.jpg next to maps.json (pictures already there are kept).
"date" is when the post went up, or when its zip last changed.

  GITHUB_TOKEN=... python3 maps_list.py old_maps.json out_dir/
"""
import datetime, hashlib, io, json, os, re, struct, sys, urllib.request, zipfile

REPO_OWNER, REPO_NAME, CATEGORY = "nzp-team", "nzportable", "Map Releases"
TOKEN = os.environ["GITHUB_TOKEN"]
MAX_ZIP = 300 * 1024 * 1024

# any direct link to a .zip: GitHub attachments, archive.org, Dropbox (dl=1)...
ZIP_RE = re.compile(r'https?://[^\s)"\'<>\]]+?\.zip(?:\?dl=[01])?(?=[\s)"\'<>\]]|$)', re.I)
LINK_RE = re.compile(r'https?://([^/\s)"\'<>\]]+)')
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
              nodes{number title url body closed createdAt author{login}
                comments(last:30){nodes{body author{login}}}}}}}""",
            {"o": REPO_OWNER, "n": REPO_NAME, "c": cat, "a": after})["repository"]["discussions"]
        yield from d["nodes"]
        if not d["pageInfo"]["hasNextPage"]:
            return
        after = d["pageInfo"]["endCursor"]


def pick_zip(body):
    urls = [u.replace("?dl=0", "?dl=1") for u in ZIP_RE.findall(body)]
    urls = [u for u in dict.fromkeys(urls) if "moddb.com" not in u and "mediafire.com" not in u]
    if not urls:
        return None
    def rank(u):
        name = u.split("?")[0].rsplit("/", 1)[-1]
        if re.search(r'pc|vril|desktop|windows', name, re.I):
            return 0
        if OTHER_PLATFORMS.search(name):
            return 2
        return 1
    urls.sort(key=rank)		# stable: the post's own order (newest first) breaks ties
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


IMG_RE = re.compile(r'!\[[^\]]*\]\((https?://[^\s)]+)\)|<img[^>]+src="(https?://[^"]+)"', re.I)
THUMB_W, THUMB_H = 256, 144


def make_thumb(body, out_dir):
    """The post's first picture as thumbs/<key>.jpg; returns the relative path or None."""
    from PIL import Image
    for m in IMG_RE.finditer(body):
        url = m.group(1) or m.group(2)
        if re.search(r'\.(gif|svg)(\?|$)', url, re.I) or "shields.io" in url:
            continue
        key = hashlib.sha1(url.encode()).hexdigest()[:12]
        rel = "thumbs/%s.jpg" % key
        path = os.path.join(out_dir, rel)
        if os.path.exists(path):
            return rel
        try:
            data = urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "nzp-ps3-maps"}), timeout=60).read(20 << 20)
            im = Image.open(io.BytesIO(data)).convert("RGB")
        except Exception as e:
            print("  picture failed:", url[:80], e)
            continue
        # fill 16:9, cutting the overhang from the middle
        scale = max(THUMB_W / im.width, THUMB_H / im.height)
        im = im.resize((max(THUMB_W, round(im.width * scale)), max(THUMB_H, round(im.height * scale))), Image.LANCZOS)
        left, top = (im.width - THUMB_W) // 2, (im.height - THUMB_H) // 2
        im = im.crop((left, top, left + THUMB_W, top + THUMB_H))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        im.save(path, "JPEG", quality=82, optimize=True)
        return rel
    return None


def main():
    old, old_threads = {}, {}
    out_dir = sys.argv[2]
    if os.path.exists(sys.argv[1]):
        try:
            prev_list = json.load(open(sys.argv[1]))
            for m in prev_list.get("maps", []) + prev_list.get("skipped", []):
                old[m["file"]] = m
                old_threads[m.get("thread")] = m
        except Exception:
            pass
    today = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    maps, skipped, seen_bsp = [], [], set()
    for d in discussions():
        # the post, then the author's own comments, newest first (updates land there)
        me = (d["author"] or {}).get("login")
        text = (d["body"] or "") + "\n" + "\n".join(
            c["body"] or "" for c in reversed(d["comments"]["nodes"]) if (c["author"] or {}).get("login") == me)
        url = pick_zip(text)
        if not url:
            hosts = sorted(set(LINK_RE.findall(text)) - {"github.com", "user-images.githubusercontent.com"})
            print("  no zip #%d %s  hosts: %s" % (d["number"], d["title"], " ".join(hosts)[:200]))
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
        # posted, or (a new zip in a thread already listed) updated today
        if prev and prev.get("date"):
            date = prev["date"]
        elif d["url"] in old_threads and old_threads[d["url"]]["file"] != url:
            date = today
        else:
            date = d.get("createdAt", "")
        entry = {
            "name": clean_title(d["title"])[:60],
            "author": (d["author"] or {}).get("login", ""),
            "version": version_of(d["title"], d["body"]),
            "file": url,
            "size": size,
            "bsp": bsp,
            "bsps": bsps,
            "desc": desc_of(d["body"] or ""),
            "thread": d["url"],
            "date": date,
        }
        thumb = make_thumb(d["body"] or "", out_dir)
        if thumb:
            entry["thumb"] = thumb
        maps.append(entry)
        print("  ok #%d %s -> %s (%d bytes)" % (d["number"], d["title"], bsp, size))
    maps.sort(key=lambda m: m["name"].lower())
    json.dump({"source": "https://github.com/%s/%s/discussions" % (REPO_OWNER, REPO_NAME),
               "maps": maps, "skipped": skipped}, open(os.path.join(out_dir, "maps.json"), "w"), indent=1)
    # pictures no map uses any more
    used = {m.get("thumb") for m in maps}
    tdir = os.path.join(out_dir, "thumbs")
    for f in os.listdir(tdir) if os.path.isdir(tdir) else []:
        if "thumbs/" + f not in used:
            os.remove(os.path.join(tdir, f))
    print("%d maps, %d skipped" % (len(maps), len(skipped)))


if __name__ == "__main__":
    main()
