#!/usr/bin/env bash
# Komut satirindan tam yeniden derleme (CubeIDE "Clean + Build" karsiligi).
#
#   tools/build.sh            URETIM derlemesi (UART_COMM_TEST KAPALI)
#   tools/build.sh test       TEST derlemesi   (-DUART_COMM_TEST)
#   tools/build.sh clean      ayni, ama once butun ara dosyalari siler
#
# NEDEN AYRI DIZIN: Debug/ dizinini STM32CubeIDE uretir ve proje her
# degistiginde YENIDEN URETIR; oraya elle eklenen her sey kaybolur (bir kez
# kaybetti). Bu betik Debug/ icindeki guncel makefile duzenini .build/ altina
# kopyalayip yalnizca UART_COMM_TEST tanimini moda gore ayarlar. Boylece
# IDE'nin ciktisi tek kaynak olmaya devam eder, iki derleme kipi de bu
# kaynaktan turer ve iki taraf birbirini ezmez.
set -u

CUBE=/c/ST/STM32CubeIDE_1.19.0/STM32CubeIDE/plugins
GCC=$(ls -d "$CUBE"/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*.win32_*/tools/bin | head -1)
MAKE=$(ls -d "$CUBE"/com.st.stm32cube.ide.mcu.externaltools.make.win32_*/tools/bin | head -1)
export PATH="$GCC:$MAKE:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/Debug"
OUT="$ROOT/.build"

MODE="uretim"
CLEAN=0
for a in "$@"; do
  [ "$a" = "test" ]  && MODE="test"
  [ "$a" = "clean" ] && CLEAN=1
done

if [ ! -f "$SRC/makefile" ]; then
  echo "HATA: $SRC/makefile yok. CubeIDE'de projeyi bir kez derleyin." >&2
  exit 1
fi

[ "$CLEAN" = "1" ] && rm -rf "$OUT"
mkdir -p "$OUT"

# IDE'nin urettigi makefile duzenini al (ara/cikti dosyalarini degil).
( cd "$SRC" && find . \( -name 'makefile' -o -name '*.mk' -o -name 'objects.list' \) -print0 ) \
  | ( cd "$SRC" && xargs -0 -I{} cp --parents {} "$OUT/" )

# Derleme kipini uygula. IDE'nin Debug yapilandirmasi -DUART_COMM_TEST
# iceriyor; uretim kipinde bu tanim cikarilir.
if [ "$MODE" = "uretim" ]; then
  find "$OUT" -name '*.mk' -exec sed -i 's/ -DUART_COMM_TEST//g' {} +
else
  find "$OUT" -name '*.mk' -exec \
    sed -i 's/-DSTM32F407xx\( -DUART_COMM_TEST\)\?/-DSTM32F407xx -DUART_COMM_TEST/g' {} +
fi

# Kaynaklar ../ ile gosterildigi icin derleme dizini proje koku altinda ve
# ayni derinlikte olmali; .build tam olarak oyle.
cd "$OUT" || exit 1
make -j8 all >build.log 2>&1
rc=$?

echo "--- uyari/hata ---"
grep -E "error:|warning:|undefined reference|multiple definition" build.log || echo "(yok)"
echo "--- UART_COMM_TEST gecen derleme satiri: $(grep -c -- '-DUART_COMM_TEST' build.log) ---"
echo "--- boyut ---"
arm-none-eabi-size ./*.elf 2>/dev/null || echo "(elf yok)"
echo "--- make rc=$rc  (kip: $MODE) ---"
exit $rc
