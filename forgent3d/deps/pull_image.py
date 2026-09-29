"""Pull a Docker Hub image's layers (linux/amd64) and unpack them into a rootfs dir, without a Docker daemon."""
import json, os, sys, tarfile, urllib.request, shutil

repo, tag, out = sys.argv[1], sys.argv[2], sys.argv[3]
layers_dir = out + ".layers"
os.makedirs(layers_dir, exist_ok=True)

def token():
    with urllib.request.urlopen(f"https://auth.docker.io/token?service=registry.docker.io&scope=repository:{repo}:pull") as r:
        return json.load(r)["token"]

def get(url, accept=None, tok=None):
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {tok or token()}", **({"Accept": accept} if accept else {})})
    return urllib.request.urlopen(req)

ACCEPT = ",".join([
    "application/vnd.docker.distribution.manifest.list.v2+json",
    "application/vnd.oci.image.index.v1+json",
    "application/vnd.docker.distribution.manifest.v2+json",
    "application/vnd.oci.image.manifest.v1+json",
])
m = json.load(get(f"https://registry-1.docker.io/v2/{repo}/manifests/{tag}", ACCEPT))
if "manifests" in m:
    pick = next(x for x in m["manifests"] if x["platform"]["architecture"] == "amd64" and x["platform"]["os"] == "linux")
    m = json.load(get(f"https://registry-1.docker.io/v2/{repo}/manifests/{pick['digest']}", ACCEPT))
json.dump(m, open(layers_dir + "/manifest.json", "w"), indent=1)
cfg = json.load(get(f"https://registry-1.docker.io/v2/{repo}/blobs/{m['config']['digest']}"))
json.dump(cfg, open(layers_dir + "/config.json", "w"), indent=1)
print("layers:", len(m["layers"]), "total", sum(l["size"] for l in m["layers"]) / 1e6, "MB", flush=True)

for i, layer in enumerate(m["layers"]):
    d = layer["digest"]
    path = f"{layers_dir}/{i:02d}-{d.split(':')[1][:12]}.tar.gz"
    if os.path.exists(path) and os.path.getsize(path) == layer["size"]:
        print("have", path, flush=True)
        continue
    print(f"get {i} {layer['size']/1e6:.1f} MB", flush=True)
    with get(f"https://registry-1.docker.io/v2/{repo}/blobs/{d}") as r, open(path + ".part", "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    os.rename(path + ".part", path)

os.makedirs(out, exist_ok=True)
for name in sorted(p for p in os.listdir(layers_dir) if p.endswith(".tar.gz")):
    print("unpack", name, flush=True)
    with tarfile.open(f"{layers_dir}/{name}") as t:
        for member in t:
            base = os.path.basename(member.name)
            parent = os.path.dirname(member.name)
            if base == ".wh..wh..opq":
                target = os.path.join(out, parent)
                if os.path.isdir(target):
                    for child in os.listdir(target):
                        p = os.path.join(target, child)
                        shutil.rmtree(p) if os.path.isdir(p) and not os.path.islink(p) else os.remove(p)
                continue
            if base.startswith(".wh."):
                target = os.path.join(out, parent, base[4:])
                if os.path.islink(target) or os.path.isfile(target):
                    os.remove(target)
                elif os.path.isdir(target):
                    shutil.rmtree(target)
                continue
            target = os.path.join(out, member.name)
            if (os.path.lexists(target)) and not (member.isdir() and os.path.isdir(target)):
                if os.path.isdir(target) and not os.path.islink(target):
                    shutil.rmtree(target)
                else:
                    os.remove(target)
            try:
                t.extract(member, out, set_attrs=not member.issym(), numeric_owner=True)
            except Exception as e:
                print("skip", member.name, e, flush=True)
print("done", flush=True)
