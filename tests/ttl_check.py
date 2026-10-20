# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Alejandro Bustos
"""Parse Turtle and compare every port with the permanent C++ ABI. Needs rdflib."""
from pathlib import Path
import re
import sys

import rdflib
from rdflib import Namespace, RDF, RDFS, URIRef

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from gen_ttl import PORTS, URI

LV2 = Namespace("http://lv2plug.in/ns/lv2core#")
ATOM = Namespace("http://lv2plug.in/ns/ext/atom#")
DOAP = Namespace("http://usefulinc.com/ns/doap#")
PPROPS = Namespace("http://lv2plug.in/ns/ext/port-props#")
UNITS = Namespace("http://lv2plug.in/ns/extensions/units#")
WORK = Namespace("http://lv2plug.in/ns/ext/worker#")
URID = Namespace("http://lv2plug.in/ns/ext/urid#")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check(bundle):
    header = (ROOT / "lv2/ports.hpp").read_text(encoding="utf-8")
    declared = re.findall(r"(\w+)\s*=\s*(\d+),\s*//\s*(\w+)", header)
    expected = {int(i): (symbol, kind) for symbol, i, kind in declared}
    require(len(declared) == len(expected) == 145 and set(expected) == set(range(145)), "C++ ABI indices/count")
    require(re.search(r"count\s*=\s*145\b", header), "C++ count sentinel")
    globals_ = "in_l in_r out_l out_r commands rec_dub overdub undo mute trigger pause next_loop dry_level loops selected quantize round mute_quant dub_quant repl_quant sync_to rel_sync eighths bpm fade safety_fb loop_secs enabled selected_out soloed free_mem cycle_clock loop_clock".split()
    suffixes = "level in_gain thresh feedback sync play_sync play_fb rate state next waiting len cycle pos".split()
    permanent = globals_ + [f"l{n}_{suffix}" for n in range(1, 9) for suffix in suffixes]
    require([expected[i][0] for i in range(145)] == permanent, "permanent Milestone 0 symbols/indices changed")
    plugin = URIRef(URI)
    manifest = rdflib.Graph().parse(bundle / "manifest.ttl", format="turtle")
    require(set(manifest.subjects(RDF.type, LV2.Plugin)) == {plugin}, "manifest plugin")
    require(manifest.value(plugin, LV2.binary) == URIRef((bundle / "loo-pi.so").as_uri()), "binary")
    require(manifest.value(plugin, RDFS.seeAlso) == URIRef((bundle / "loo-pi.ttl").as_uri()), "seeAlso")
    graph = rdflib.Graph().parse(bundle / "loo-pi.ttl", format="turtle")
    require((plugin, RDF.type, LV2.Plugin) in graph and (plugin, RDF.type, LV2.DelayPlugin) in graph, "plugin class")
    require(graph.value(plugin, DOAP.license) == URIRef("https://spdx.org/licenses/GPL-2.0-or-later"), "license")
    require(set(graph.objects(plugin, LV2.requiredFeature)) == {URID.map, WORK.schedule}, "required features")
    require(WORK.interface in set(graph.objects(plugin, LV2.extensionData)), "worker interface")
    require(LV2.hardRTCapable in set(graph.objects(plugin, LV2.optionalFeature)), "RT declaration")
    require(int(graph.value(plugin, LV2.minorVersion)) == 1 and int(graph.value(plugin, LV2.microVersion)) == 0, "version")
    require(URIRef("http://lv2plug.in/ns/ext/options#options") in set(graph.objects(plugin, LV2.optionalFeature)), "host block options")
    actual, symbols = {}, set()
    for node in graph.objects(plugin, LV2.port):
        index = int(graph.value(node, LV2["index"]))
        symbol, name = str(graph.value(node, LV2.symbol)), str(graph.value(node, LV2.name))
        require(index not in actual and symbol not in symbols, "duplicate index/symbol")
        symbols.add(symbol)
        require(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol), f"symbol {symbol}")
        require(name.isascii() and 0 < len(name) <= 16, f"name {symbol}")
        types = set(graph.objects(node, RDF.type))
        kinds = types & {LV2.AudioPort, LV2.ControlPort, LV2.CVPort, ATOM.AtomPort}
        directions = types & {LV2.InputPort, LV2.OutputPort}
        require(len(kinds) == len(directions) == 1, f"type {symbol}")
        kind = {LV2.AudioPort: "audio", LV2.ControlPort: "control", LV2.CVPort: "cv", ATOM.AtomPort: "atom"}[next(iter(kinds))]
        kind += "_in" if LV2.InputPort in types else "_out"
        actual[index] = (symbol, kind)
        p = PORTS[index]
        require((symbol, kind) == (p["symbol"], p["kind"]), f"generator ABI {symbol}")
        for key, predicate in [("lo", LV2.minimum), ("hi", LV2.maximum), ("default", LV2.default)]:
            value = graph.value(node, predicate)
            require((value is None and p[key] is None) or (value is not None and float(value) == p[key]), f"{symbol}: {key}")
        props = {str(value) for value in graph.objects(node, LV2.portProperty)}
        expected_props = {str((LV2 if prop.startswith("lv2:") else PPROPS)[prop.split(":")[1]]) for prop in p["props"]}
        require(props == expected_props, f"properties {symbol}")
        points = sorted((int(graph.value(point, RDF.value)), str(graph.value(point, RDFS.label)))
                        for point in graph.objects(node, LV2.scalePoint))
        require(points == list(enumerate(p["labels"])), f"scale points {symbol}")
        require(graph.value(node, UNITS.unit) == (UNITS[p["unit"]] if p["unit"] else None), f"unit {symbol}")
        if symbol == "commands":
            require(graph.value(node, ATOM.bufferType) == ATOM.Sequence and set(graph.objects(node, ATOM.supports)) == {ATOM.Int}, "command buffer")
        if symbol == "enabled":
            require(graph.value(node, LV2.designation) == LV2.enabled, "enabled designation")
    require(actual == expected, "Turtle differs from ports.hpp")
    print("PASS ttl_check: all 145 ports, metadata, features and permanent ABI")


if __name__ == "__main__":
    try:
        check(Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "loo-pi.lv2").resolve())
    except Exception as error:
        sys.exit(f"FAIL ttl_check: {error}")
