#!/usr/bin/env bash
# Tam yeniden derleme (CubeIDE "Project > Clean" + "Build Project" karsiligi).
# Kullanim: tools/build.sh [clean] [test]
#   test  -> -DUART_COMM_TEST (dogrulama kosuculari ve hata enjeksiyon
#            kancalari derlenir). Bayrak yoksa URETIM derlemesidir ve test
#            kodu binary'e hic girmez.
# Tam gunluk: Debug/build.log ; bu betik yalnizca uyari/hata ozetini basar.
set -u
CUBE=/c/ST/STM32CubeIDE_1.19.0/STM32CubeIDE/plugins
GCC=$(ls -d "$CUBE"/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*.win32_*/tools/bin | head -1)
MAKE=$(ls -d "$CUBE"/com.st.stm32cube.ide.mcu.externaltools.make.win32_*/tools/bin | head -1)
export PATH="$GCC:$MAKE:$PATH"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/Debug" || exit 1
TESTFLAG=""
for a in "$@"; do
  [ "$a" = "test" ] && TESTFLAG="-DUART_COMM_TEST"
done

if [ "${1:-}" = "clean" ]; then
  find . \( -name '*.o' -o -name '*.d' -o -name '*.su' -o -name '*.cyclo' \) -delete
  rm -f ./*.elf ./*.map UART_IDLE_DMAv2.list   # objects.list ASLA silinmez
fi
make -j8 all CFLAGS_EXTRA="$TESTFLAG" >build.log 2>&1
rc=$?
echo "--- uyari/hata ---"
grep -E "error:|warning:|undefined reference|multiple definition" build.log || echo "(yok)"
echo "--- boyut ---"
arm-none-eabi-size UART_IDLE_DMAv2.elf 2>/dev/null || echo "(elf yok)"
echo "--- make rc=$rc  (UART_COMM_TEST: ${TESTFLAG:-kapali}) ---"
exit $rc
