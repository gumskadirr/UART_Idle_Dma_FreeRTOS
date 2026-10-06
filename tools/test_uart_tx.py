"""Gercek RX/TX C kodunun HAL modeliyle TX davranis testleri."""
import os
import pathlib
import subprocess
from test_uart_rx import ROOT

CASES = ("dma_error_reported", "line_preserves_tx", "done_and_error",
         "idle_error_ignored", "event_during_take", "start_busy", "start_error_safe",
         "start_error_active", "invalid_length", "source_copy", "complete_before_return",
         "error_during_start", "timeout_and_reuse", "result_blocks_send",
         "missing_abort_callback", "abort_callback_with_en", "line_not_quiet",
         "late_done_abort_and_fault", "completed_before_late_service",
         "done_during_timeout_decision", "tick_wrap", "start_failure_result",
         "ht_clear_completion_race")


def main():
    cc = pathlib.Path(r"C:\Program Files (x86)\Atollic\TrueSTUDIO for STM32 9.3.0\PCTools\bin\gcc.exe")
    env = os.environ.copy()
    env["PATH"] = str(cc.parent) + os.pathsep + env.get("PATH", "")
    out = ROOT / ".build" / "host-tests"
    out.mkdir(parents=True, exist_ok=True)
    executable = out / "tx_tests.exe"
    subprocess.run([str(cc), "-std=c99", "-Wall", "-Wextra", "-Werror", "-DUART_COMM_TEST",
                    "-Itools/tests/hal_model", "-ICore/Inc", "-ILib/Uart", "tools/tests/test_uart_tx.c",
                    "Lib/Uart/uart_comm.c", "Lib/Uart/uart_comm_port.c", "tools/tests/hal_model/uart_callbacks.c", "Core/Src/protocol.c", "Core/Src/protocol_uart.c", "-o", str(executable)],
                   cwd=ROOT, env=env, check=True)
    failed = 0
    for name in CASES:
        result = subprocess.run([str(executable), name], cwd=ROOT, env=env,
                                capture_output=True, text=True)
        print(f"{'FAIL' if result.returncode else 'PASS'} {name}")
        if result.returncode:
            failed += 1
            print(result.stderr.strip())
    print(f"TX HAL modeli: {len(CASES) - failed}/{len(CASES)} PASS")
    return bool(failed)


if __name__ == "__main__":
    raise SystemExit(main())
