"""Gercek F407 HAL+FreeRTOS ile, protokolsuz USART1 ARM link kontrolu.

Karta yukleme yapmaz. Kaynaklar ayni Lib/Uart dosyalaridir; yalniz proje
ayar header'lari ignored .build/portable/config altina kopyalanir.
"""
import os
import pathlib
import shutil
import subprocess
from test_uart_rx import ROOT

def main():
    plugins = pathlib.Path('C:/ST/STM32CubeIDE_1.19.0/STM32CubeIDE/plugins')
    toolchain = next(plugins.glob('com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*.win32_*/tools/bin'))
    gcc = toolchain / 'arm-none-eabi-gcc.exe'
    nm = toolchain / 'arm-none-eabi-nm.exe'
    out = ROOT / '.build/portable'
    config = out / 'config'
    config.mkdir(parents=True, exist_ok=True)
    for name in ('FreeRTOSConfig.h', 'stm32f4xx_hal_conf.h'):
        shutil.copyfile(ROOT / 'Core/Inc' / name, config / name)
    env = os.environ.copy()
    env['PATH'] = str(toolchain) + os.pathsep + env.get('PATH', '')
    kernel = 'Middlewares/Third_Party/FreeRTOS/Source'
    includes = [config, ROOT / 'Lib/Uart', ROOT / 'Drivers/STM32F4xx_HAL_Driver/Inc',
                ROOT / 'Drivers/CMSIS/Device/ST/STM32F4xx/Include', ROOT / 'Drivers/CMSIS/Include',
                ROOT / kernel / 'include', ROOT / kernel / 'portable/GCC/ARM_CM4F']
    flags = ['-mcpu=cortex-m4', '-mthumb', '-mfpu=fpv4-sp-d16', '-mfloat-abi=hard', '-Os',
             '-std=c99', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
             '-DUSE_HAL_DRIVER', '-DSTM32F407xx'] + ['-I' + str(p) for p in includes]
    sources = ['Lib/Uart/uart_comm.c', 'Lib/Uart/uart_comm_port.c', 'tools/tests/test_uart_portable.c',
               'Core/Src/system_stm32f4xx.c', 'Core/Src/syscalls.c', 'Core/Src/sysmem.c',
               'Core/Startup/startup_stm32f407vgtx.s']
    sources += [kernel + '/' + p for p in ('tasks.c', 'queue.c', 'list.c', 'timers.c',
                'portable/GCC/ARM_CM4F/port.c', 'portable/MemMang/heap_4.c')]
    sources += ['Drivers/STM32F4xx_HAL_Driver/Src/' + p for p in
                ('stm32f4xx_hal.c', 'stm32f4xx_hal_uart.c', 'stm32f4xx_hal_dma.c',
                 'stm32f4xx_hal_cortex.c', 'stm32f4xx_hal_rcc.c')]
    objects = []
    log = out / 'build.log'
    with log.open('w', encoding='utf-8') as stream:
        for index, source in enumerate(sources):
            obj = out / ('source_%02d.o' % index)
            command = [str(gcc)] + flags + ['-c', str(ROOT / source), '-o', str(obj)]
            result = subprocess.run(command, cwd=ROOT, env=env, stdout=stream, stderr=stream)
            if result.returncode:
                print(log.read_text(encoding='utf-8'))
                return 1
            objects.append(str(obj))
        elf = out / 'uart_usart1.elf'
        command = [str(gcc)] + flags + objects + ['-T' + str(ROOT / 'STM32F407VGTX_FLASH.ld'),
                   '--specs=nano.specs', '--specs=nosys.specs', '-Wl,--gc-sections',
                   '-Wl,-Map=' + str(out / 'uart_usart1.map'), '-o', str(elf)]
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=stream, stderr=stream)
    if result.returncode:
        print(log.read_text(encoding='utf-8'))
        return 1
    symbols = subprocess.check_output([str(nm), str(elf)], env=env, text=True)
    (out / 'symbols.log').write_text(symbols, encoding='utf-8')
    names = [line.split()[-1] for line in symbols.splitlines() if line.split()]
    # frame_dummy is GCC runtime bookkeeping, not this project's protocol.
    if any(name.startswith(('frame_parser_', 'frame_build', 'crc16_', 'protocol_', 'app_protocol_')) for name in names):
        raise AssertionError('Portable UART contains protocol dependency')
    core_undefined = subprocess.check_output([str(nm), '-u', objects[0]], env=env, text=True)
    (out / 'uart-undefined.log').write_text(core_undefined, encoding='utf-8')
    if any(prefix in core_undefined for prefix in ('frame_', 'crc16_', 'protocol_', 'app_protocol_')):
        raise AssertionError('UART core unresolved protocol dependency')
    print('PASS protocol-free USART1/DMA2 ARM integration link')
    # Invalid config must fail specifically at the package's own checks.
    cases = [('UART_RX_BUF_SIZE=0', 'UART_RX_BUF_SIZE'), ('UART_RX_BUF_SIZE=255', 'UART_RX_BUF_SIZE'),
             ('UART_RX_SCRATCH_SIZE=257', 'UART_RX_SCRATCH_SIZE'), ('UART_RX_SERVICE_BUDGET=0', 'UART_RX_SERVICE_BUDGET'),
             ('UART_TX_BUF_SIZE=0', 'UART_TX_BUF_SIZE'), ('COMM_QUEUE_SIZE=0', 'COMM_QUEUE_SIZE'),
             ('COMM_STACK_SIZE=0', 'COMM_STACK_SIZE'), ('COMM_TASK_PRIORITY=56', 'COMM_TASK_PRIORITY'),
             ('UART_RX_TIMEOUT_MS=0', 'UART'), ('UART_TX_TIMEOUT_MS=0x80000000U', 'UART')]
    for define, expected in cases:
        result = subprocess.run([str(gcc)] + flags + ['-D' + define, '-c', str(ROOT / sources[0]),
                                '-o', str(out / 'invalid.o')], cwd=ROOT, env=env, capture_output=True, text=True)
        if result.returncode == 0 or expected not in result.stderr or '#error' not in result.stderr:
            print(result.stderr)
            raise AssertionError('Invalid config not rejected: ' + define)
    print('PASS invalid config rejected (%d cases)' % len(cases))
    print('Portable integration: 2/2 PASS (link + config checks)')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
