"""CH340 acceptance: real host RX/TX, no PA2-PA3 loopback.

Requires the serial test firmware (tools/build.sh serial), existing pyserial,
and TX->PA3, RX->PA2, GND->GND at 3.3V / 115200 8N1.
"""
import argparse
import binascii
import json
import pathlib
import struct
import threading
import time

import serial

ROOT = pathlib.Path(__file__).resolve().parent.parent


def frame(kind, seq, payload=b""):
    body = struct.pack("<BBBH", 1, kind, len(payload), seq & 65535) + payload
    return b"\xaa\x55" + body + struct.pack("<H", binascii.crc_hqx(body, 65535))


class Link:
    def __init__(self, port):
        self.port = serial.Serial(port, 115200, timeout=0.02, write_timeout=15)
        self.buffer = bytearray()
        self.frames = []
        self.errors = []
        self.lock = threading.Lock()
        self.running = True
        self.port.reset_input_buffer()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        try:
            while self.running:
                self.buffer.extend(self.port.read(max(1, self.port.in_waiting)))
                while len(self.buffer) >= 9:
                    start = self.buffer.find(b"\xaa\x55")
                    if start < 0:
                        self.errors.append(f"unexpected TX bytes: {bytes(self.buffer[:-1]).hex()}")
                        del self.buffer[:-1]
                        break
                    if start:
                        self.errors.append(f"unexpected TX bytes: {bytes(self.buffer[:start]).hex()}")
                        del self.buffer[:start]
                    if len(self.buffer) < 9:
                        break
                    size = self.buffer[4] + 9
                    if size > 64 or self.buffer[2] != 1:
                        self.errors.append("bad header")
                        del self.buffer[0]
                        continue
                    if len(self.buffer) < size:
                        break
                    raw = bytes(self.buffer[:size])
                    if binascii.crc_hqx(raw[2:-2], 65535) != struct.unpack("<H", raw[-2:])[0]:
                        self.errors.append("bad CRC")
                        del self.buffer[0]
                        continue
                    with self.lock:
                        self.frames.append((raw[3], struct.unpack("<H", raw[5:7])[0], raw[7:-2]))
                    del self.buffer[:size]
        except Exception as exc:
            self.errors.append(str(exc))

    def send(self, raw):
        if self.port.write(raw) != len(raw):
            raise AssertionError("short serial write")

    def count(self, kind):
        with self.lock:
            return sum(f[0] == kind for f in self.frames)

    def take(self, kind):
        with self.lock:
            return [f for f in self.frames if f[0] == kind]

    def wait(self, kind, count, timeout=2):
        end = time.monotonic() + timeout
        while time.monotonic() < end and self.count(kind) < count:
            time.sleep(0.002)
        assert self.count(kind) == count, f"type {kind:02x}: expected {count}, received {self.count(kind)}"
        assert not self.errors, self.errors

    def stats(self, idle_ms=0):
        base = self.count(0x73)
        cmd = 3 if idle_ms else 2
        self.send(frame(0x71, 0, struct.pack("<BHBI", cmd, idle_ms, 0, 0)))
        self.wait(0x73, base + 2, timeout=idle_ms / 1000 + 3)
        pages = self.take(0x73)[-2:]
        assert [p[2][0] for p in pages] == [0, 1]
        return [list(struct.unpack("<" + "I" * ((len(p[2]) - 1) // 4), p[2][1:])) for p in pages]

    def close(self):
        if not self.running:
            return
        time.sleep(0.03)  # Son USB/seri parcasi da okuyucuya ulassin.
        self.running = False
        self.reader.join(1)
        self.port.close()
        if self.buffer:
            self.errors.append(f"trailing TX bytes: {bytes(self.buffer).hex()}")


def check_echo(link, raw, expected):
    base = link.count(0x70)
    link.send(raw)
    link.wait(0x70, base + len(expected))
    assert link.take(0x70)[base:] == expected


def run(link, probe=False, report=None):
    if report is None:
        report = []

    def passed(name, **values):
        report.append(dict(test=name, status="PASS", **values))
        print("PASS", name, values, flush=True)

    literal = b"\x01\x02\x03\x04"
    check_echo(link, frame(0x70, 42, literal), [(0x70, 42, literal)])
    passed("external_bidirectional_echo")
    if probe:
        return report
    raw = frame(0x70, 43, bytes(range(55)))
    base = link.count(0x70)
    for chunk in (raw[:1], raw[1:5], raw[5:31], raw[31:]):
        link.send(chunk)
        time.sleep(0.006)
    link.wait(0x70, base + 1)
    assert link.take(0x70)[-1] == (0x70, 43, bytes(range(55)))
    passed("fragmented_max64")
    expected = [(0x70, 44 + i, bytes([i])) for i in range(3)]
    check_echo(link, b"".join(frame(*f) for f in expected), expected)
    passed("combined_frames")
    bad = bytearray(frame(0x70, 50, b"bad")); bad[-1] ^= 1
    check_echo(link, b"garbage" + bad + frame(0x70, 51, b"good"), [(0x70, 51, b"good")])
    passed("bad_crc_resync")
    before = link.stats()
    partial = frame(0x70, 52, b"incomplete")
    link.send(partial[:5]); time.sleep(0.09)
    check_echo(link, frame(0x70, 53, b"new"), [(0x70, 53, b"new")])
    after = link.stats()
    assert after[0][8] > before[0][8]
    passed("partial_timeout_resync", timeout_delta=after[0][8] - before[0][8])
    expected = [(0x70, (65500 + i) & 65535, bytes([i & 255])) for i in range(520)]
    for pos in range(0, len(expected), 4):
        batch = expected[pos:pos + 4]
        check_echo(link, b"".join(frame(*f) for f in batch), batch)
    passed("520_frames_dma_and_sequence_wrap")
    base = link.count(0x70); before = link.stats()
    link.send(frame(0x71, 2000, struct.pack("<BHBI", 4, 0, 0, 0)))
    link.wait(0x70, base + 8)
    assert [f[1] for f in link.take(0x70)[base:]] == list(range(2000, 2008))
    after = link.stats()
    assert after[0][2] - before[0][2] == 1 and after[0][11] - before[0][11] == 1
    passed("queue_full_reply_drop_visible", accepted=8, dropped=1)
    idle = link.stats(10000)
    assert idle[1][0] == idle[1][1] == idle[1][2] == idle[1][3] == 0
    passed("idle_10s_no_uart_wake", cycles=0, iterations=0)

    for name, count, size, period in [("full_duplex_100hz_10s", 1000, 13, 10),
                                       ("full_duplex_continuous64", 1000, 64, 0)]:
        before = link.stats(); base = link.count(0x72)
        start = time.perf_counter()
        link.send(frame(0x71, 0, struct.pack("<BHBI", 1, period, size, count)))
        incoming = [frame(0x74, i, bytes((i + j) & 255 for j in range(size - 9))) for i in range(count)]
        if period:
            for i, packet in enumerate(incoming):
                remaining = start + i * period / 1000 - time.perf_counter()
                if remaining > 0:
                    time.sleep(remaining)
                link.send(packet)
        else:
            link.send(b"".join(incoming))
        link.wait(0x72, base + count, timeout=15)
        time.sleep(0.05)
        after = link.stats(); dt = after[1][11] - before[1][11]
        outgoing = link.take(0x72)[base:]
        assert outgoing == [(0x72, i, bytes((i + j) & 255 for j in range(size - 9))) for i in range(count)]
        assert after[0][4] == before[0][4] and after[0][6] == before[0][6]
        assert after[0][7] == before[0][7] and after[0][10] == before[0][10]
        assert after[0][9] - before[0][9] == count
        assert after[0][5] - before[0][5] == count
        cycles = sum((after[1][i] - before[1][i]) & 0xffffffff for i in (0, 1))
        cpu = cycles / (168000 * dt) * 100
        if period:
            assert cpu <= 2, f"UART CPU {cpu:.3f}% > 2%"
        assert after[1][7] >= 128 and after[1][4] < 1680 and after[1][6] < 840000
        passed(name, host_tx=count, host_rx=count, elapsed_ms=dt, uart_cpu_percent=round(cpu, 3),
               stack_free_words=after[1][7], max_critical_cycles=after[1][4], max_rx_latency_cycles=after[1][6])
    before = link.stats()
    link.port.send_break(0.03); time.sleep(0.1)
    check_echo(link, frame(0x70, 3000, b"recovered"), [(0x70, 3000, b"recovered")])
    after = link.stats()
    assert after[0][12] > before[0][12], "physical break did not trigger RX restart"
    passed("external_break_rx_recovery", restart_delta=after[0][12] - before[0][12])
    assert not link.errors
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", default="COM18")
    p.add_argument("--probe", action="store_true")
    p.add_argument("--metrics-only", action="store_true", help="Read cumulative IRQ/handler/stack limits after the load suite")
    p.add_argument("--sink-order-probe", action="store_true", help="Inject duplicate RX sequence and wrong size; both must be detected")
    p.add_argument("--output", type=pathlib.Path, default=ROOT / ".build/board-tests/ch340-results.json")
    args = p.parse_args()
    link = Link(args.port)
    report, error = [], None
    expected = 1 if args.probe or args.metrics_only or args.sink_order_probe else 11
    try:
        if args.sink_order_probe:
            before = link.stats(); base = link.count(0x72)
            link.send(frame(0x71, 0, struct.pack("<BHBI", 1, 10, 13, 4)))
            packets = [frame(0x74, i, bytes((i + j) & 255 for j in range(n)))
                       for i, n in [(0, 4), (1, 4), (1, 4), (3, 1)]]
            link.send(b"".join(packets)); link.wait(0x72, base + 4)
            time.sleep(0.05); after = link.stats()
            bad = after[0][10] - before[0][10]
            assert bad == 2, f"duplicate sequence / wrong payload size: expected 2 rejected frames, found {bad}"
            report.append(dict(test="rx_duplicate_and_wrong_size_detected", status="PASS", mismatch_delta=bad))
            print("PASS", report[-1], flush=True)
        elif args.metrics_only:
            values = link.stats()[1]
            assert values[4] < 1680 and values[5] < 168000 and values[6] < 840000, f"profile={values}"
            assert values[7] >= 128 and values[8] < 16800 and values[9] < 16800, f"profile={values}"
            report.append(dict(test="irq_handler_stack_limits", status="PASS", max_critical_cycles=values[4],
                               max_irq_cycles=values[5], max_rx_latency_cycles=values[6], stack_free_words=values[7],
                               max_frame_handler_cycles=values[8], max_result_handler_cycles=values[9],
                               max_service_cycles=values[10]))
            print("PASS", report[-1], flush=True)
        else:
            run(link, args.probe, report)
        assert len(report) == expected, "incomplete acceptance run"
        link.close()
        assert not link.errors, link.errors
        print(f"RESULT {len(report)}/{len(report)} PASS; {args.output}")
    except Exception as exc:
        error = str(exc)
        raise
    finally:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(dict(port=args.port, baud=115200, status="FAIL" if error else "PASS",
                                             expected=expected, passed=len(report),
                                             error=error, tests=report), indent=2), encoding="utf-8")
        link.close()


if __name__ == "__main__":
    main()
