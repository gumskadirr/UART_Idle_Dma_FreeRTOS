"""STM32F4 portu: gercek esleme, secilen IRQ ve gecersiz stream kontrolleri."""
import os
import pathlib
import subprocess
from test_uart_rx import ROOT

def main():
    cc = pathlib.Path(r'C:\Program Files (x86)\Atollic\TrueSTUDIO for STM32 9.3.0\PCTools\bin\gcc.exe')
    env = os.environ.copy()
    env['PATH'] = str(cc.parent) + os.pathsep + env.get('PATH', '')
    out = ROOT / '.build/host-tests/port_tests.exe'
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(cc), '-std=c99', '-Wall', '-Wextra', '-Werror', '-DUART_PORT_MODEL_ONLY',
                    '-Itools/tests/hal_model', '-ICore/Inc', '-ILib/Uart',
                    'tools/tests/test_uart_port.c', 'Lib/Uart/uart_comm_port.c',
                    'tools/tests/hal_model/uart_callbacks.c', '-o', str(out)], cwd=ROOT, env=env, check=True)
    subprocess.run([str(out)], cwd=ROOT, env=env, check=True)
    print('STM32F4 port modeli: 2/2 PASS')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
