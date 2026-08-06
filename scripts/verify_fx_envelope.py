#!/usr/bin/env python3
"""Live-verify doc 136 — envelope.ensure_fx_envelope, the FX-param authoring seam.

Exercises, end-to-end on the real SDK path against a running REAPER, the pre-registered
expectations E-136.1 .. E-136.9 (preregistration-136.md):

  E-136.1  GetFXEnvelope(create=true) returns an envelope for a never-automated param
  E-136.2  "ensure" is idempotent — a second call creates nothing
  E-136.3  the created envelope is GetTrackEnvelopeByName-visible (predicted REFUTED:
           a point-less envelope may be pruned, exactly as the built-in path's seed exists for)
  E-136.4  a fresh envelope has 0 points  (coupled to E-136.3 by construction)
  E-136.5  same-named params on two FX make the by-name add_point seam UNSAFE
  E-136.6  GetEnvelopeName == TrackFX_GetParamName (predicted REFUTED: REAPER may decorate)
  E-136.7  THE THESIS — the envelope this verb creates is the one spatial.export_adm samples
  E-136.9  negative control: bad indices REFUSE and create nothing

The pure halves — tiered param-name resolution and the name-collision count — are proven
deterministically off-REAPER in unit.fx_envelope, including the negative control that
reproduces the naive first-substring defect. This gate proves the VERB runs on real REAPER
against a real VST3 panner, and that its output is load-bearing for the ADM exporter.

E-136.7's discriminator is deliberately an INDEPENDENT witness: spatial.export_adm's
`positionSource` field takes exactly three values ("none" / "static" / "envelope") and predates
this batch by many cycles. The baseline reading before the verb runs is what makes the reading
after it informative — without it, "envelope" proves nothing about causation.

It opens its own scratch tracks, so the user's project is untouched.

Usage:
    python3 verify_fx_envelope.py            # auto-find reaper_mcp.json
    python3 verify_fx_envelope.py --cleanup  # remove the scratch tracks at the end
    python3 verify_fx_envelope.py /path/to/reaper_mcp.json
"""
import json
import os
import sys
import urllib.request

DISCOVERY_DEFAULTS = [
    os.path.expanduser("~/Library/Application Support/REAPER/reaper_mcp.json"),
    os.path.expanduser("~/.config/REAPER/reaper_mcp.json"),
]
EXPECTED_SURFACE = 190  # 189 (post-B4) + 1 (ensure_fx_envelope)
# REAPER's Add-FX search matches the SCANNED DISPLAY name, which for the IEM suite is
# "StereoEncoder (IEM)" — not "IEM StereoEncoder". The VST3: prefix pins the format so the
# 64ch VST2 build in the same cache cannot be picked instead.
PANNER = "VST3:StereoEncoder (IEM)"
AZ_PARAM = "Azimuth Angle"

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = {a for a in sys.argv[1:] if a.startswith("--")}
DO_CLEANUP = "--cleanup" in flags

path = args[0] if args else next((p for p in DISCOVERY_DEFAULTS if os.path.exists(p)), None)
if not path or not os.path.exists(path):
    sys.exit("reaper_mcp.json not found — pass its path, or run the 'reaper_mcp: status' action "
             "first. (Discovery is written at load; if it is absent, REAPER is not running the "
             "extension.)")

disc = json.load(open(path))
URL, TOKEN = disc["url"], disc["token"]
HEADERS = {"Content-Type": "application/json", "Authorization": "Bearer " + TOKEN}
print("endpoint : %s\ntoken    : %s...\nversion  : %s\n" % (URL, TOKEN[:8], disc.get("version")))

_id = 0
def next_id():
    global _id
    _id += 1
    return _id

def rpc(method, params=None):
    body = json.dumps({"jsonrpc": "2.0", "id": next_id(), "method": method,
                       "params": params or {}}).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers=HEADERS)
    with urllib.request.urlopen(req, timeout=120) as r:
        j = json.loads(r.read())
    if "error" in j:
        raise RuntimeError("%s -> %s" % (method, j["error"]))
    return j["result"]

def call(tool_name, **arguments):
    r = rpc("tools/call", {"name": tool_name, "arguments": arguments})
    if r.get("isError"):
        raise RuntimeError("%s isError: %s" % (tool_name, r.get("content")))
    return r.get("structuredContent", {})

def raw(tool_name, **arguments):
    return rpc("tools/call", {"name": tool_name, "arguments": arguments})

def refused(result):
    """True when the call failed CLOSED — an error, not a silent success."""
    sc = result.get("structuredContent", {})
    return result.get("isError") is True or (isinstance(sc, dict) and sc.get("error") is not None)

def refusal_text(result):
    sc = result.get("structuredContent", {})
    if isinstance(sc, dict) and sc.get("detail"):
        return str(sc["detail"])
    c = result.get("content")
    if isinstance(c, list) and c and isinstance(c[0], dict):
        return str(c[0].get("text", ""))[:200]
    return json.dumps(sc)[:200]

passed = failed = 0
def check(cond, label, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", label, ("  - " + detail) if detail else ""))

def note(label, detail=""):
    print("  [note] %s%s" % (label, ("  - " + detail) if detail else ""))

def add_track(name, nchan=None):
    idx = call("project.get_summary").get("trackCount", 0)
    t = call("track.add", index=idx, name=name).get("trackIndex", idx)
    if nchan:
        call("spatial.set_track_channels", track=t, channels=nchan)
    return t

def env_count(track):
    return call("envelope.list", track=track).get("envelopeCount", 0)

# ================================================================================================
rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                   "clientInfo": {"name": "verify_fx_envelope", "version": "0"}})

print("== 0. surface >= %d ==" % EXPECTED_SURFACE)
enum = call("tools.enumerate", profile="all")
surface = enum.get("count", enum.get("toolCount", 0))
names = {t.get("name") for t in enum.get("tools", [])}
check(surface >= EXPECTED_SURFACE, "surface is >= %d on the wire" % EXPECTED_SURFACE, "got %s" % surface)
check("envelope.ensure_fx_envelope" in names, "envelope.ensure_fx_envelope is registered")

print("\n== 1. scratch session: a bed and one object track with a real VST3 panner ==")
# Start from an EMPTY project so track indices are deterministic and the user's session is
# untouched. project.new refuses (never blocks) if the current project is dirty.
newp = raw("project.new")
if refused(newp):
    note("project.new declined — running in the current project", refusal_text(newp))
else:
    print("  [note] started an empty scratch project")
bed = add_track("d136 bed", 6)
obj = add_track("Object 1", 1)
added = call("fx.add", track=obj, name=PANNER)
fx = added.get("fxIndex", 0)
check(fx >= 0, "%s instantiated on the object track" % PANNER, "fxIndex=%s name=%s"
      % (fx, added.get("name")))
chain = call("fx.list", track=obj)
nparams = next((f.get("numParams") for f in chain.get("fx", []) if f.get("index") == fx), 0)
check(nparams > 0, "panner exposes parameters", "numParams=%s" % nparams)
check(env_count(obj) == 0, "object track starts with ZERO envelopes — the baseline")

print("\n== 2. BASELINE CONTROL — with no envelope, the ADM exporter reads 'static' ==")
print("     (without this reading, 'envelope' later would prove nothing about causation)")
# The window is PINNED (boundsFlag=0) rather than left at the default ENTIRE PROJECT. A scratch
# project has no items, so its length is 0 and the sampler plans exactly one block whatever the
# automation does — a reading that says nothing about the envelope. Pinning it here and at §7
# keeps the two calls identical in every respect but the one variable under test.
ADM_WINDOW = dict(boundsFlag=0, startPos=0.0, endPos=4.0)
base = call("spatial.export_adm", bedTrack=bed, bedLayout="5.1", objectTracks=[obj],
            dryRun=True, **ADM_WINDOW)
base_obj = (base.get("objects") or [{}])[0]
base_src, base_blocks = base_obj.get("positionSource"), base_obj.get("blocks")
check(base_src == "static", "baseline positionSource == 'static'", "got %r" % base_src)
print("     baseline blocks over the same 0..4s window: %s" % base_blocks)

print("\n== 3. E-136.1 / E-136.3 / E-136.4 / E-136.6 — the create ==")
r = call("envelope.ensure_fx_envelope", track=obj, fx=fx, paramName=AZ_PARAM)
check(r.get("ok") is True, "E-136.1 HOLDS: create returned an envelope")
check(r.get("created") is True, "envelope did not exist before — this call created it")
env_name = r.get("envelope")
param_name = r.get("paramName")
print("     GetEnvelopeName    : %r" % env_name)
print("     TrackFX_GetParamName: %r" % param_name)
if env_name == param_name:
    check(True, "E-136.6 HOLDS: envelope name == param name")
else:
    check(True, "E-136.6 REFUTED (as predicted): REAPER DECORATES the envelope name",
          "%r != %r — returning the param name would have broken the add_point seam" %
          (env_name, param_name))
check(r.get("matchedBy") == "exact", "paramName matched at the EXACT tier",
      "matchedBy=%r" % r.get("matchedBy"))

check(r.get("seeded") is False and r.get("nameVisible") is True,
      "E-136.3 HOLDS: the created envelope is name-visible WITHOUT our seed",
      "seeded=%s — the fallback seed path did not run" % r.get("seeded"))
# E-136.4 predicted 0 points, and predicted that a count of 1 could only mean OUR seed fired.
# Both halves are wrong: REAPER creates the FX envelope already carrying one point at the
# parameter's CURRENT value — the same sonic no-op the built-in chunk path has to construct by
# hand. That is also WHY E-136.3 holds: REAPER never hands back the point-less envelope that
# would have been pruned. Measured below rather than asserted.
check(r.get("pointCount") == 1,
      "E-136.4 REFUTED: REAPER PRE-SEEDS the envelope — 1 point, not 0",
      "pointCount=%s with seeded=false, so the point is REAPER's, not ours" % r.get("pointCount"))
seed_pts = call("envelope.get_points", track=obj, envelope=env_name).get("points", [])
cur_norm = call("fx.get_param", track=obj, fx=fx, param=r.get("param")).get("normalized")
if seed_pts:
    p0 = seed_pts[0]
    check(p0.get("time") == 0.0 and abs((p0.get("value") or 0) - (cur_norm or 0)) < 1e-9,
          "REAPER's pre-seed sits at t=0 with the param's CURRENT normalized value",
          "point=%s current=%s -> activation is sonically a no-op" % (p0.get("value"), cur_norm))
check(r.get("nameVisible") is True, "the returned name IS resolvable by GetTrackEnvelopeByName")
check(r.get("nameResolvesToThis") is True, "and it resolves to THIS envelope, not another")
check(r.get("nameCollisionCount") == 1, "exactly one envelope carries the name")
check(r.get("addPointSafe") is True, "addPointSafe:true — the seam is usable")

print("\n== 4. E-136.2 — 'ensure' is idempotent ==")
before = env_count(obj)
r2 = call("envelope.ensure_fx_envelope", track=obj, fx=fx, paramName=AZ_PARAM)
after = env_count(obj)
check(r2.get("created") is False, "E-136.2 HOLDS: second call reports created:false")
check(after == before, "envelope count unchanged", "%s -> %s" % (before, after))
check(r2.get("envelope") == env_name, "same envelope name returned")
check(r2.get("pointCount") == r.get("pointCount"), "no extra point added",
      "%s -> %s" % (r.get("pointCount"), r2.get("pointCount")))

print("\n== 5. name-resolution tiers, live on a real roster ==")
for req, tier in ((AZ_PARAM, "exact"), (AZ_PARAM.lower(), "case-insensitive"), ("azim", "substring")):
    rr = call("envelope.ensure_fx_envelope", track=obj, fx=fx, paramName=req)
    check(rr.get("matchedBy") == tier and rr.get("envelope") == env_name,
          "%-16r -> %s tier, same envelope" % (req, tier), "matchedBy=%r" % rr.get("matchedBy"))

print("\n== 6. E-136.9 — the negative controls: the gate CAN fail ==")
n_before = env_count(obj)
cases = [
    ("fx index out of range",   dict(track=obj, fx=99, paramName=AZ_PARAM)),
    ("param index out of range", dict(track=obj, fx=fx, param=9999)),
    ("ambiguous paramName 'Angle'", dict(track=obj, fx=fx, paramName="Angle")),
    ("no such paramName",       dict(track=obj, fx=fx, paramName="Nonexistent Param")),
    ("both param and paramName", dict(track=obj, fx=fx, param=0, paramName=AZ_PARAM)),
    ("neither param nor paramName", dict(track=obj, fx=fx)),
]
for label, kwargs in cases:
    res = raw("envelope.ensure_fx_envelope", **kwargs)
    check(refused(res), "REFUSES: %s" % label, refusal_text(res))
check(env_count(obj) == n_before, "no stray envelope created by ANY refusal",
      "%s -> %s" % (n_before, env_count(obj)))

print("\n== 7. E-136.7 — THE THESIS: does the ADM exporter sample THIS envelope? ==")
# Sweep azimuth in the param's NORMALIZED [0,1] scale (IEM: 0.5 = 0 deg, 0.75 = +90).
for t_s, v in ((0.0, 0.25), (2.0, 0.50), (4.0, 0.75)):
    call("envelope.add_point", track=obj, envelope=env_name, time=t_s, value=v)
pts = call("envelope.get_points", track=obj, envelope=env_name)
check(pts.get("pointCount", 0) >= 3, "three trajectory points written through add_point",
      "pointCount=%s" % pts.get("pointCount"))

# Identical to the §2 baseline call in every argument — the ONLY thing that changed between
# them is that the envelope now exists and carries a trajectory.
after_adm = call("spatial.export_adm", bedTrack=bed, bedLayout="5.1", objectTracks=[obj],
                 dryRun=True, **ADM_WINDOW)
after_obj = (after_adm.get("objects") or [{}])[0]
after_src, after_blocks = after_obj.get("positionSource"), after_obj.get("blocks")
check(after_src == "envelope",
      "E-136.7 HOLDS: positionSource 'static' -> 'envelope' on one changed variable",
      "the exporter now samples the envelope this verb created — B3 Phase 2 is UNBLOCKED")
check(after_blocks > base_blocks,
      "and the planned block count rose with it",
      "%s -> %s over the same 0..4s window" % (base_blocks, after_blocks))

tl = raw("analysis.object_decode_timeline", objects=[obj], boundsFlag=0, startPos=0.0, endPos=4.0)
if refused(tl):
    note("decode timeline declined", refusal_text(tl))
else:
    sc = tl.get("structuredContent", {})
    o0 = (sc.get("objects") or [{}])[0]
    mig = o0.get("migration") or []
    travel = o0.get("travelDeg", o0.get("angularTravelDeg"))
    check(len(mig) > 1 or (travel or 0) > 1.0,
          "second witness: the object actually MOVES across the decode",
          "migration=%s travelDeg=%s" % (len(mig), travel))

print("\n== 8. E-136.5 — the collision that makes the by-name seam unsafe ==")
added2 = call("fx.add", track=obj, name=PANNER)
fx2 = added2.get("fxIndex", 1)
check(fx2 != fx, "a SECOND %s on the same track" % PANNER, "fxIndex=%s" % fx2)
r3 = call("envelope.ensure_fx_envelope", track=obj, fx=fx2, paramName=AZ_PARAM)
check(r3.get("nameCollisionCount", 0) >= 2,
      "E-136.5 CONFIRMED: two envelopes now carry %r" % r3.get("envelope"),
      "nameCollisionCount=%s" % r3.get("nameCollisionCount"))
check(r3.get("addPointSafe") is False,
      "addPointSafe:false — the verb REPORTS the hazard instead of picking",
      "a caller trusting the name here would automate the wrong FX with ok:true")

if DO_CLEANUP:
    print("\n== cleanup ==")
    for t in sorted([bed, obj], reverse=True):
        try:
            call("track.remove", track=t)
        except Exception as e:
            print("  (could not remove track %s: %s)" % (t, e))

print("\n%d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
