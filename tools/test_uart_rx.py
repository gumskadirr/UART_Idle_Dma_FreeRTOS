"""Gercek C kodunu mevcut PC GCC ile HAL/register modeli uzerinde sinar.

Yeni kutuphane gerekmez. Donanim NDTR/TC sirasi ve gecikmesi bu testle
dogrulanmaz; onlar PA2-PA3 loopback ve kart olcumleri gerektirir.
Kullanim: python tools/test_uart_rx.py [--cc gcc-yolu]
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CASES = (
    "normal", "active_start", "budget", "overrun", "pending_tc", "sample_retry",
    "timeout_sample_stuck", "partial_timeout", "continuation", "healthy_idle",
    "retry_exhausted", "missing_callback", "callback_with_en", "stopped_irq",
    "unhealthy_start", "overwrite_copy", "error_copy", "primask_preserved",
    "healthy_start_irq", "fault_preserves_tx_irq",
    "rebind", "sync_start_error", "producer_boundaries", "full_lap_and_300",
    "restart_progress", "error_each_restart", "timeout_rearm", "early_fault_late_service",
    "valid_frame_closes_recovery", "cold_start_recovers", "app_snapshot",
    "raw_bytes", "reset_discards_partial", "timeout_progress_and_wrap", "validated_with_new_error",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc")
    args = parser.parse_args()
    cc = args.cc or shutil.which("gcc")
    if not cc:
        bundled = pathlib.Path(r"C:\Program Files (x86)\Atollic\TrueSTUDIO for STM32 9.3.0\PCTools\bin\gcc.exe")
        if bundled.is_file():
            cc = str(bundled)
    if not cc:
        parser.error("PC GCC bulunamadi; --cc ile mevcut derleyiciyi belirtin.")
    output = ROOT / ".build" / "host-tests"
    output.mkdir(parents=True, exist_ok=True)
    executable = output / ("rx_tests.exe" if sys.platform == "win32" else "rx_tests")
    command = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-DUART_COMM_TEST",
               "-Itools/tests/hal_model", "-ICore/Inc", "tools/tests/test_uart_rx.c",
               "Core/Src/uart_comm.c", "Core/Src/protocol.c", "Core/Src/protocol_uart.c", "Core/Src/app_protocol.c", "-o", str(executable)]
    env = os.environ.copy()
    env["PATH"] = str(pathlib.Path(cc).resolve().parent) + os.pathsep + env.get("PATH", "")
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    failed = []
    for name in CASES:
        result = subprocess.run([str(executable), name], cwd=ROOT, env=env, capture_output=True, text=True)
        print(f"{'FAIL' if result.returncode else 'PASS'} {name}")
        if result.returncode:
            failed.append(name)
            print(result.stderr.strip())
    print(f"HAL/register modeli: {len(CASES) - len(failed)}/{len(CASES)} PASS")
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
