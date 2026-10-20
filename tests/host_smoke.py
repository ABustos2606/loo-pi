# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Alejandro Bustos
"""Milestone 2 host checks, adapted from Discipline's private JACK test.

Needs jackd, jalv, Ingen, python3-jack-client and numpy. Leaves running user
servers alone. Logs are written under build/host-smoke/.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time

import jack

URI = "https://github.com/ABustos2606/loo-pi#loopi"
PREFIXES = '''@prefix patch: <http://lv2plug.in/ns/ext/patch#> .
@prefix ingen: <http://drobilla.net/ns/ingen#> .
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
'''


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def wait_for(check, message):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.05)
    raise RuntimeError(message)


class Protocol:
    def __init__(self, path, log):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.connect(str(path))
        self.socket.settimeout(10)
        self.buffer = b""
        self.sequence = 0
        self.log = log

    def request(self, verb, fields):
        self.sequence += 1
        message = PREFIXES + f'[] a patch:{verb} ; patch:sequenceNumber "{self.sequence}"^^xsd:int ; {fields} .\n'
        self.socket.sendall(message.encode())
        self.log.write("SEND " + message)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            while b"\0" not in self.buffer:
                self.socket.settimeout(max(0.01, deadline - time.monotonic()))
                data = self.socket.recv(65536)
                require(data, "Ingen closed its socket")
                self.buffer += data
            data, self.buffer = self.buffer.split(b"\0", 1)
            text = data.decode()
            self.log.write("RECV " + text + "\n")
            self.log.flush()
            for response in re.findall(r"a patch:Response\s*;(.*?)\.\s*(?:\n|$)", text, re.S):
                seq = re.search(r'patch:sequenceNumber "(\d+)"', response)
                status = re.search(r'patch:body "(-?\d+)"', response)
                if seq and status and int(seq[1]) == self.sequence:
                    require(int(status[1]) == 0, f"Ingen {verb}: {text}")
                    return text
        raise RuntimeError(f"Ingen {verb} timed out")

    def put(self, path, body):
        return self.request("Put", f"patch:subject <{path}> ; patch:body [ {body} ]")

    def connect(self, tail, head):
        self.put("ingen:/main/", f"a ingen:Arc ; ingen:tail <{tail}> ; ingen:head <{head}>")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--ingen", default="ingen")
    args = parser.parse_args()
    logdir = Path("build/host-smoke").resolve()
    logdir.mkdir(parents=True, exist_ok=True)
    name = f"loopi-test-{os.getpid()}"
    private_lv2 = logdir / f"lv2-{os.getpid()}"
    shutil.copytree(args.bundle.resolve(), private_lv2 / "loo-pi.lv2")
    env = dict(os.environ, JACK_DEFAULT_SERVER=name, JACK_NO_AUDIO_RESERVATION="1")
    env["LV2_PATH"] = str(private_lv2) + ":" + env.get("LV2_PATH", "/usr/local/lib/lv2:/usr/lib/lv2")
    processes, logs, paths = [], [], []
    probe = protocol = None

    def launch(label, command):
        log = (logdir / (label + ".log")).open("w")
        logs.append(log)
        proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, env=env)
        processes.append(proc)
        return proc

    def stop(proc):
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)

    try:
        launch("jack", ["jackd", "--no-realtime", "--timeout", "5000", "--name", name,
                        "-d", "dummy", "-r", "48000", "-p", "1024"])
        wait_for(lambda: subprocess.run(["jack_lsp"], env=env, stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL).returncode == 0, "JACK did not start")
        probe = jack.Client("loopi_probe", servername=name, no_start_server=True)
        sources = [probe.outports.register(s) for s in ("src_l", "src_r")]
        captures = [probe.inports.register(s) for s in ("cap_l", "cap_r")]
        observed = [0.0, 0.0]
        source_level = [0.125, -0.25]

        @probe.set_process_callback
        def process(frames):
            for source, value in zip(sources, source_level):
                source.get_array()[:] = value
            for i, capture in enumerate(captures):
                observed[i] = float(capture.get_array().mean())

        probe.activate()

        def cable(client):
            for side, source, capture in zip(("l", "r"), sources, captures):
                probe.connect(source, f"{client}:in_{side}")
                probe.connect(f"{client}:out_{side}", capture)

        host = launch("jalv", ["jalv", "-i", "-p", "-n", "loopi_jalv", "-x", URI])
        wait_for(lambda: bool(probe.get_ports("loopi_jalv:out_l")), "jalv plugin ports missing")
        time.sleep(0.5)
        require(host.poll() is None, "jalv activation failed")
        cable("loopi_jalv")
        wait_for(lambda: observed == [0.125, -0.25], "jalv stereo dry mismatch")
        stop(host)
        print("PASS jalv: discovery, required features, stereo dry audio", flush=True)

        for threads in (1, 4):
            path = Path(f"/tmp/{name}-{threads}.sock")
            paths.append(path)
            client = f"loopi_ingen_{threads}"
            engine = launch(f"ingen-{threads}", [args.ingen, "-e", "-f", "-s", name, "-n", client,
                                               "-S", str(path), "-E", "0", "-p", str(threads)])
            wait_for(path.exists, "Ingen socket missing")
            time.sleep(1)
            require(engine.poll() is None, "Ingen activation failed")
            log = (logdir / f"protocol-{threads}.log").open("w")
            logs.append(log)
            protocol = Protocol(path, log)
            for side in ("l", "r"):
                protocol.put(f"ingen:/main/in_{side}", "a lv2:InputPort, lv2:AudioPort")
                protocol.put(f"ingen:/main/out_{side}", "a lv2:OutputPort, lv2:AudioPort")
            protocol.put("ingen:/main/loopi", f"a ingen:Block ; lv2:prototype <{URI}>")
            discovered = protocol.request("Get", "patch:subject <ingen:/main/loopi>")
            require(all(symbol in discovered for symbol in ("commands", "l8_pos", "dry_level")), "Ingen full port discovery")
            for side in ("l", "r"):
                protocol.connect(f"ingen:/main/in_{side}", f"ingen:/main/loopi/in_{side}")
                protocol.connect(f"ingen:/main/loopi/out_{side}", f"ingen:/main/out_{side}")
            wait_for(lambda: bool(probe.get_ports(f"{client}:out_l")), "Ingen JACK ports missing")
            cable(client)
            wait_for(lambda: observed == [0.125, -0.25], "Ingen dry mismatch")

            def control(symbol, value):
                protocol.request("Set", f'patch:subject <ingen:/main/loopi/{symbol}> ; '
                                 f'patch:property ingen:value ; patch:value "{value}"^^xsd:float')

            control("dry_level", 0.5)
            wait_for(lambda: observed == [0.0625, -0.125], "Ingen dry level mismatch")
            control("enabled", 0)
            time.sleep(0.1)
            require(observed == [0.0625, -0.125], "Ingen port bypass dry mismatch")
            control("enabled", 1)
            control("dry_level", 1)
            wait_for(lambda: observed == [0.125, -0.25], "Ingen control restore mismatch")

            # Record a real JACK test signal through the command control. More
            # than 0.5 s between Rec/Dub presses avoids its double-tap gesture.
            control("safety_fb", 0)
            control("dry_level", 0)
            control("rec_dub", 1)
            time.sleep(0.05)
            control("rec_dub", 0)
            time.sleep(0.8)
            control("rec_dub", 1)
            time.sleep(0.05)
            control("rec_dub", 0)
            source_level[:] = [0, 0]
            wait_for(lambda: all(abs(a - b) < 0.00001 for a, b in zip(observed, (0.125, -0.25))), "Ingen recorded loop playback")
            time.sleep(1)  # A complete return with live input silent.
            require(all(abs(a - b) < 0.00001 for a, b in zip(observed, (0.125, -0.25))), "Ingen loop persists without live input")
            control("enabled", 0)
            wait_for(lambda: observed == [0, 0], "Ingen loop bypass mute")
            control("enabled", 1)
            wait_for(lambda: all(abs(a - b) < 0.00001 for a, b in zip(observed, (0.125, -0.25))), "Ingen loop bypass restore")
            control("undo", 1)
            time.sleep(1.1)
            control("undo", 0)
            wait_for(lambda: observed == [0, 0], "Ingen long Undo Clear")
            source_level[:] = [0.125, -0.25]

            def atom(command, press=True, target=0):
                value = target * 256 + command * 2 + int(press)
                protocol.request("Set", 'patch:subject <ingen:/main/loopi/commands> ; '
                                 f'patch:property ingen:value ; patch:value "{value}"^^xsd:int')

            atom(5)  # Dedicated Record, not the Rec/Dub double-tap gesture.
            time.sleep(0.05)
            atom(5, False)
            time.sleep(0.3)
            atom(5)
            time.sleep(0.05)
            atom(5, False)
            source_level[:] = [0, 0]
            wait_for(lambda: all(abs(a - b) < 0.00001 for a, b in zip(observed, (0.125, -0.25))), "Ingen atom Record playback")
            atom(0)  # Undo the one recorded layer.
            wait_for(lambda: observed == [0, 0], "Ingen atom Undo")
            atom(0, False)
            atom(1)  # Redo is available through the atom input in M1.
            wait_for(lambda: all(abs(a - b) < 0.00001 for a, b in zip(observed, (0.125, -0.25))), "Ingen atom Redo")
            atom(1, False)
            atom(17)  # Clear / Undo all.
            wait_for(lambda: observed == [0, 0], "Ingen atom Clear")
            # Real Ingen worker allocation, targets, mix, solo and removal.
            control("loop_secs", 10)
            control("loops", 4)
            time.sleep(0.4)
            source_level[:] = [0.03125, -0.0625]
            atom(5, target=1)
            time.sleep(0.2)
            atom(5, False, target=1)
            atom(5, target=1)
            time.sleep(0.05)
            atom(5, False, target=1)
            source_level[:] = [0, 0]
            wait_for(lambda: all(abs(a-b)<0.00001 for a,b in zip(observed,(0.125,-0.25))), "Ingen four-loop atom mix")
            atom(24, target=3)
            atom(24, False, target=3)
            wait_for(lambda: all(abs(a-b)<0.00001 for a,b in zip(observed,(0.03125,-0.0625))), "Ingen explicit loop 2 solo")
            atom(24, target=3)
            atom(24, False, target=3)
            wait_for(lambda: all(abs(a-b)<0.00001 for a,b in zip(observed,(0.125,-0.25))), "Ingen solo restore")
            control("loops", 1)
            wait_for(lambda: all(abs(a-b)<0.00001 for a,b in zip(observed,(0.03125,-0.0625))), "Ingen remove playing loops")
            # Milestone 4: the loops are saved with the patch, as Peepo saves
            # it (patch:Copy to a bundle), and come back Paused in a new engine.
            control("loops", 2)
            time.sleep(0.4)
            source_level[:] = [0.0625, -0.125]
            atom(5, target=3)
            time.sleep(0.15)
            atom(5, False, target=3)
            atom(5, target=3)
            time.sleep(0.05)
            atom(5, False, target=3)
            source_level[:] = [0, 0]
            mix = (0.09375, -0.1875)
            wait_for(lambda: all(abs(a-b)<0.00001 for a,b in zip(observed,mix)), "Ingen second loop before saving")
            bundle = logdir / f"saved-{os.getpid()}-{threads}.ingen"
            protocol.request("Copy", f"patch:subject <ingen:/main> ; patch:destination <{bundle.as_uri()}>")
            state = bundle / "loopi"
            wait_for(lambda: (state / "state.ttl").exists(), "Ingen saved no Loo-pi state")
            require((state / "loop1.wav").exists() and (state / "loop2.wav").exists(), "Ingen saved loop files")
            protocol.socket.close()
            stop(engine)
            path = Path(f"/tmp/{name}-{threads}-reload.sock")
            paths.append(path)
            client = f"loopi_ingen_{threads}_reload"
            engine = launch(f"ingen-{threads}-reload", [args.ingen, "-e", "-f", "-s", name, "-n", client,
                                                      "-S", str(path), "-E", "0", "-p", str(threads)])
            wait_for(path.exists, "Ingen reload socket missing")
            time.sleep(1)
            log = (logdir / f"protocol-{threads}-reload.log").open("w")
            logs.append(log)
            protocol = Protocol(path, log)
            protocol.request("Copy", f"patch:subject <{bundle.as_uri()}> ; patch:destination <ingen:/main>")
            wait_for(lambda: bool(probe.get_ports(f"{client}:out_l")), "Ingen reloaded JACK ports missing")
            cable(client)
            time.sleep(0.5)
            require(observed == [0, 0], "Ingen reloaded loops are not paused")
            atom(15, target=1)  # Trigger, all loops.
            atom(15, False, target=1)
            wait_for(lambda: all(abs(a-b)<0.00001 for a,b in zip(observed,mix)), "Ingen reloaded loops play")
            atom(17, target=1)
            wait_for(lambda: observed == [0,0], "Ingen final clear")
            source_level[:] = [0.125, -0.25]
            protocol.socket.close()
            protocol = None
            stop(engine)
            print(f"PASS Ingen: four-loop worker/mix/targets/solo/remove, controls/atoms/bypass, {threads} threads", flush=True)
    finally:
        if protocol:
            protocol.socket.close()
        if probe:
            probe.deactivate()
            probe.close()
        for proc in reversed(processes):
            stop(proc)
            if proc.stdin:
                proc.stdin.close()
        for log in logs:
            log.close()
        for path in paths:
            path.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
