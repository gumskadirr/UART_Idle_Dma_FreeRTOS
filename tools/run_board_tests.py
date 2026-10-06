"""Mevcut CubeIDE ST-LINK/GDB ile test ELF'ini karta yukler, sonuclari okur.

Yalniz test firmware'i icindir. PA2-PA3 jumper'i gerekir.
Loglar .build/board-tests altinda tutulur; harici paket gerektirmez.
"""
import argparse
import pathlib
import re
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PLUGINS = pathlib.Path(r"C:\ST\STM32CubeIDE_1.19.0\STM32CubeIDE\plugins")


def find(pattern):
    return next(PLUGINS.glob(pattern))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stop")
    parser.add_argument("--elf", type=pathlib.Path, default=ROOT / ".build" / "UART_IDLE_DMAv2.elf")
    parser.add_argument("--production", action="store_true", help="Test kancalari kapali firmware'in scheduler/RX baslangic kontrolu")
    parser.add_argument("--serial", action="store_true", help="CH340 fixture firmware'ini yukle ve public RX/TX baslangicini kontrol et")
    args = parser.parse_args()
    out = ROOT / ".build" / "board-tests"
    out.mkdir(parents=True, exist_ok=True)
    server = find("com.st.stm32cube.ide.mcu.externaltools.stlink-gdb-server.*/tools/bin/ST-LINK_gdbserver.exe")
    gdb = find("com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin/arm-none-eabi-gdb.exe")
    programmer = find("com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.*/tools/bin/STM32_Programmer_CLI.exe")
    script = out / "read-tests.gdb"
    stop = args.stop or ("StartDefaultTask" if args.production or args.serial else "uart_rtos_test_done")
    prefix = [
        "set pagination off", "set confirm off", "set print elements 0",
        "set remotetimeout 10", "target extended-remote localhost:61235",
        "monitor reset", "load", "monitor reset",
        "tbreak " + stop, "continue"]
    if args.production or args.serial:
        label = "SERIAL" if args.serial else "PRODUCTION"
        commands = ["p comm.published", "p uxCurrentNumberOfTasks",
                    f'printf "{label} initialized=%u rx_ready=%u tx_accepting=%u tx_state=%u tasks=%u\\n", comm.published.initialized, comm.published.rx_ready, comm.published.tx_accepting, comm.published.tx_state, uxCurrentNumberOfTasks']
    else:
        commands = ["p test_sayisi", "p test_gecen", "p test_kalan",
        "p lb_sayisi", "p lb_gecen", "p lb_kalan", "p lb_calismayan", "p lb_sonuc",
        "p uart_comm_test_sayisi", "p uart_comm_test_gecen", "p uart_comm_test_kalan",
        "p uart_comm_test_kosmayan", "p uart_comm_test_atlanan",
        "p uart_comm_test_kayit", "p rx_stats", "p app_proto_state",
        "p comm", "p huart2", "p hdma_usart2_rx", "p *hdma_usart2_rx.Instance",
        "p *huart2.Instance", "p tx_stats",
        "p rtos_profile", "p rtos_idle_iterations", "p rtos_idle_cycles",
        "p rtos_100hz_cpu_permille", "p rtos_stream_cpu_permille",
        "p rtos_100hz_frames", "p rtos_stream_frames",
        "p app_protocol_test_max_critical_cycles",
        'printf "COUNTS unit=%u/%u loopback=%u/%u acceptance=%u/%u fail=%u not_run=%u skip=%u\\n", test_gecen, test_sayisi, lb_gecen, lb_sayisi, uart_comm_test_gecen, uart_comm_test_sayisi, uart_comm_test_kalan, uart_comm_test_kosmayan, uart_comm_test_atlanan',
        ]
    script.write_text("\n".join(prefix + commands + ["detach", "quit", ""]), encoding="utf-8")
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    with (out / "server.log").open("w") as log:
        process = subprocess.Popen([str(server), "-d", "-p", "61235", "-s",
                                    "-cp", str(programmer.parent)], cwd=ROOT,
                                   stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
        try:
            time.sleep(2)
            if process.poll() is not None:
                log.flush()
                print((out / "server.log").read_text())
                return 1
            result = subprocess.run([str(gdb), "--batch", "-x", str(script),
                                     str(args.elf.resolve())],
                                    cwd=ROOT, capture_output=True, text=True, timeout=90,
                                    creationflags=flags)
            (out / "results.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if args.stop or result.returncode:
                print(result.stdout + result.stderr)
                return result.returncode
            if args.production or args.serial:
                report = re.search(label + r" .*", result.stdout)
                print(report.group(0) if report else result.stdout + result.stderr)
                print(f"Tam debugger kaydi: {out / 'results.log'}")
                return int(not re.search(label + r" initialized=1 rx_ready=1 tx_accepting=1 tx_state=0 tasks=4", result.stdout))
            counts = re.search(r"COUNTS .*", result.stdout)
            if counts:
                print(counts.group(0))
            failed = re.findall(r'id = 0x[0-9a-f]+ "([^"]+)", result = ([023])', result.stdout)
            for name, status in failed:
                print(f"{'FAIL' if status == '2' else 'NOT_RUN/SKIP'} {name}")
            print(f"Tam debugger kaydi: {out / 'results.log'}")
            verified = re.search(r"COUNTS unit=(\d+)/(\d+) loopback=(\d+)/(\d+) acceptance=(\d+)/(\d+) fail=(\d+) not_run=(\d+) skip=(\d+)", result.stdout)
            if not verified:
                return 1
            up, ut, lp, lt, ap, at, af, nr, sk = map(int, verified.groups())
            return int(bool(failed) or ut != 29 or up != ut or lt != 15 or lp != lt or at < 49 or ap != at or af or nr or sk)
        finally:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    raise SystemExit(main())
