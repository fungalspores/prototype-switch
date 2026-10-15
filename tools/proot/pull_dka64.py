import json, urllib.request, os, sys, tarfile
repo = "devkitpro/devkita64"
out = "/opt/dka64-rootfs"
def get(url, tok=None, accept=None):
    r = urllib.request.Request(url)
    if tok: r.add_header("Authorization", "Bearer " + tok)
    if accept: r.add_header("Accept", accept)
    return urllib.request.urlopen(r, timeout=120)
tok = json.load(get(f"https://auth.docker.io/token?service=registry.docker.io&scope=repository:{repo}:pull"))["token"]
idx = json.load(get(f"https://registry-1.docker.io/v2/{repo}/manifests/latest", tok,
    "application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.list.v2+json"))
dig = [m for m in idx["manifests"] if m["platform"].get("architecture") == "arm64"][0]["digest"]
man = json.load(get(f"https://registry-1.docker.io/v2/{repo}/manifests/{dig}", tok,
    "application/vnd.oci.image.manifest.v1+json, application/vnd.docker.distribution.manifest.v2+json"))
layers = man["layers"]
print("layers:", len(layers), "total MB:", sum(l["size"] for l in layers) // 2**20, flush=True)
os.makedirs(out, exist_ok=True)
for i, l in enumerate(layers):
    p = f"/tmp/dka_layer{i}.tgz"
    if not os.path.exists(p) or os.path.getsize(p) != l["size"]:
        with get(f"https://registry-1.docker.io/v2/{repo}/blobs/{l['digest']}", tok) as r, open(p, "wb") as f:
            while True:
                b = r.read(1 << 20)
                if not b: break
                f.write(b)
    print("layer", i, l["size"] // 2**20, "MB", flush=True)
    with tarfile.open(p) as t:
        for m in t:
            n = os.path.basename(m.name)
            if n.startswith(".wh."):
                tgt = os.path.join(out, os.path.dirname(m.name), n[4:])
                os.system(f"rm -rf '{tgt}'"); continue
            if m.isdev(): continue
            try: t.extract(m, out, set_attrs=False)
            except Exception as e: pass
    os.remove(p)
print("done")
