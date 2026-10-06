"""Birlesik gercek C modulu + deterministik RTOS modeli; kart scheduler testi ayridir."""
import os
import pathlib
import subprocess
from test_uart_rx import ROOT

CASES = ('fifo', 'validation', 'wrong_dma', 'epoch_fault', 'epoch_recovery', 'early_result', 'notify_context',
         'callback_reenqueue', 'task_wait_race', 'combined_error', 'fresh_wait', 'backlog_abort', 'counter_wrap',
         'suspended_notify', 'deferred_recovery_one', 'deferred_recovery_eight')
def main():
    cc = pathlib.Path(r'C:\Program Files (x86)\Atollic\TrueSTUDIO for STM32 9.3.0\PCTools\bin\gcc.exe')
    env = os.environ.copy()
    env['PATH'] = str(cc.parent) + os.pathsep + env.get('PATH', '')
    out = ROOT / '.build/host-tests/comm_tests.exe'
    subprocess.run([str(cc), '-std=c99', '-Wall', '-Wextra', '-Werror', '-DUART_COMM_TEST', '-DUART_RTOS_MODEL',
                    '-Itools/tests/rtos_model', '-Itools/tests/hal_model', '-ICore/Inc',
                    'tools/tests/test_uart_comm.c', 'Core/Src/uart_comm.c', 'Core/Src/protocol.c', '-o', str(out)], cwd=ROOT, env=env, check=True)
    failed = 0
    for case in CASES:
        result = subprocess.run([str(out), case], capture_output=True, text=True, env=env, cwd=ROOT)
        print(('FAIL' if result.returncode else 'PASS') + ' ' + case)
        if result.returncode:
            failed += 1
            print(result.stderr.strip())
    print(f'RTOS modeli: {len(CASES)-failed}/{len(CASES)} PASS')
    return bool(failed)
if __name__ == '__main__':
    raise SystemExit(main())
