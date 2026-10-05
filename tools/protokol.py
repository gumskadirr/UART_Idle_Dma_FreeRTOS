#!/usr/bin/env python3
"""
UART protokolunun bagimsiz referans uygulamasi.

Amac: STM32 tarafindaki C kodunun uretimini/cozumunu dogrulamak ve
test vektorleri uretmek. Bu dosya ileride PC gonderme tarafinin
cekirdegi olarak da kullanilabilir.

Paket: AA 55 | VERSION | TYPE | LENGTH | SEQUENCE(2) | PAYLOAD | CRC16(2)
  LENGTH  : yalnizca payload boyutu, toplam = 9 + LENGTH
  CRC     : VERSION'dan payload sonuna kadar, CRC-16/CCITT-FALSE
  Cok baytli alanlar little-endian.

Kullanim:
  python tools/protokol.py test               kendi testlerini kosar
  python tools/protokol.py vektor             C icin test dizileri uretir
  python tools/protokol.py akis               ayristirici test akislarini uretir
  python tools/protokol.py coz "AA 55 01 ..."  bayt dizisini cozer ve dogrular
"""
import sys

BAS1, BAS2 = 0xAA, 0x55
SURUM      = 0x01
EK_BOYU    = 9
MAX_PAYLOAD = 55
MAX_BOYUT   = 64

TURLER = {
    0x10: "joystick verisi",
    0x11: "joystick modu",
    0x20: "cikis ayarla",
    0x80: "komut yaniti",
}


def crc16_ccitt(veri):
    """CRC-16/IBM-3740 (CCITT-FALSE): poli 0x1021, init 0xFFFF, yansitma yok."""
    crc = 0xFFFF
    for b in veri:
        crc ^= (b << 8)
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def u16(deger):
    """16 bit degeri little-endian iki bayta cevirir."""
    return bytes([deger & 0xFF, (deger >> 8) & 0xFF])


def paket_olustur(tur, sira, payload=b""):
    """Tam paketi uretir. C tarafindaki paket_olustur ile ayni davranir."""
    payload = bytes(payload)
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload %d bayt, en fazla %d olabilir" % (len(payload), MAX_PAYLOAD))
    govde = bytes([SURUM, tur, len(payload)]) + u16(sira) + payload
    return bytes([BAS1, BAS2]) + govde + u16(crc16_ccitt(govde))


def joystick_paketi(x, y, sira):
    """X ve Y isaretli 16 bit; isaretsize cevrilip little-endian yazilir."""
    return paket_olustur(0x10, sira, u16(x & 0xFFFF) + u16(y & 0xFFFF))


def paket_coz(veri):
    """Bayt dizisini cozer. Sonuc: (gecerli_mi, rapor_satirlari)."""
    d = bytes(veri)
    r = []
    hata = []

    r.append("Toplam uzunluk : %d bayt" % len(d))
    if len(d) < EK_BOYU:
        hata.append("Paket cok kisa: en az %d bayt gerekli" % EK_BOYU)
        return False, r + hata

    r.append("Baslangic      : %02X %02X" % (d[0], d[1]))
    if d[0] != BAS1 or d[1] != BAS2:
        hata.append("Baslangic isareti yanlis, %02X %02X bekleniyordu" % (BAS1, BAS2))

    r.append("VERSION        : %d" % d[2])
    if d[2] != SURUM:
        hata.append("Surum %d, bu cozucu %d bekliyor" % (d[2], SURUM))

    tur = d[3]
    r.append("TYPE           : 0x%02X  (%s)" % (tur, TURLER.get(tur, "BILINMEYEN")))

    uzunluk = d[4]
    beklenen_toplam = EK_BOYU + uzunluk
    r.append("LENGTH         : %d  -> toplam %d bayt olmali" % (uzunluk, beklenen_toplam))
    if uzunluk > MAX_PAYLOAD:
        hata.append("LENGTH %d, en fazla %d olabilir" % (uzunluk, MAX_PAYLOAD))
    if len(d) != beklenen_toplam:
        hata.append("Uzunluk uyusmuyor: %d bayt var, %d bekleniyor" % (len(d), beklenen_toplam))
        return False, r + hata

    sira = d[5] | (d[6] << 8)
    r.append("SEQUENCE       : %d  (baytlar %02X %02X)" % (sira, d[5], d[6]))

    payload = d[7:7 + uzunluk]
    r.append("PAYLOAD        : %s" % (" ".join("%02X" % b for b in payload) if payload else "(yok)"))

    gelen_crc = d[7 + uzunluk] | (d[8 + uzunluk] << 8)
    hesap_crc = crc16_ccitt(d[2:7 + uzunluk])
    r.append("CRC (gelen)    : 0x%04X  (hatta %02X %02X)" % (gelen_crc, d[7 + uzunluk], d[8 + uzunluk]))
    r.append("CRC (hesap)    : 0x%04X  <- VERSION..payload sonu, %d bayt" % (hesap_crc, uzunluk + 5))
    if gelen_crc != hesap_crc:
        hata.append("CRC UYUSMUYOR - paket bozuk, komut uygulanmamali")

    # Bilinen turler icin payload yorumu
    if tur == 0x10:
        if uzunluk == 4:
            x = int.from_bytes(payload[0:2], "little", signed=True)
            y = int.from_bytes(payload[2:4], "little", signed=True)
            r.append("  -> joystick X = %d, Y = %d" % (x, y))
        else:
            hata.append("Joystick paketi 4 bayt payload bekler, %d geldi" % uzunluk)
    elif tur == 0x11:
        if uzunluk == 1:
            r.append("  -> joystick modu = %s" % ("acik" if payload[0] else "kapali"))
        elif uzunluk != 0:
            hata.append("Joystick modu 1 bayt payload bekler, %d geldi" % uzunluk)

    return (len(hata) == 0), r + hata


def hex_coz(metin):
    """'AA 55 01' / '0xAA,0x55' / 'aa5501' bicimlerini bayt dizisine cevirir."""
    t = metin.replace("0x", " ").replace("0X", " ")
    for ch in ",;\t\r\n[]{}":
        t = t.replace(ch, " ")
    parcalar = t.split()
    if len(parcalar) == 1 and len(parcalar[0]) % 2 == 0:
        parcalar = [parcalar[0][i:i + 2] for i in range(0, len(parcalar[0]), 2)]
    return bytes(int(p, 16) for p in parcalar)


def c_dizisi(ad, veri):
    govde = ",".join("0x%02X" % b for b in veri)
    return "static const uint8_t %s[%d] = {%s};" % (ad, len(veri), govde)


def komut_vektor():
    ornekler = [
        ("b_joystick", joystick_paketi(1000, -500, 1)),
        ("b_mod",      paket_olustur(0x11, 2, bytes([0x01]))),
        ("b_bos",      paket_olustur(0x11, 7)),
        ("b_aa55",     paket_olustur(0x20, 300, bytes([0xAA, 0x55]))),
    ]
    for ad, p in ornekler:
        print(c_dizisi(ad, p))


def komut_test():
    gecen = kalan = 0

    def kontrol(ad, bulunan, beklenen):
        nonlocal gecen, kalan
        if bulunan == beklenen:
            gecen += 1
            print("[PASS] %s" % ad)
        else:
            kalan += 1
            print("[FAIL] %s\n       beklenen: %s\n       bulunan : %s" % (ad, beklenen, bulunan))

    kontrol("CRC \"123456789\"", crc16_ccitt(b"123456789"), 0x29B1)
    kontrol("CRC bos", crc16_ccitt(b""), 0xFFFF)
    kontrol("joystick paketi",
            joystick_paketi(1000, -500, 1).hex(" ").upper(),
            "AA 55 01 10 04 01 00 E8 03 0C FE 46 59")
    kontrol("payload'siz paket uzunlugu", len(paket_olustur(0x11, 7)), 9)
    kontrol("AA 55 payload'li paket uzunlugu",
            len(paket_olustur(0x20, 300, bytes([0xAA, 0x55]))), 11)

    # Uret-coz turu: urettigimiz her paket kendi cozucumuzden gecmeli
    for ad, p in [("joystick", joystick_paketi(-1000, 32767, 65535)),
                  ("mod", paket_olustur(0x11, 0, bytes([0x00]))),
                  ("cikis", paket_olustur(0x20, 1, bytes([0x03, 0x01])))]:
        gecerli, _ = paket_coz(p)
        kontrol("uret-coz: %s" % ad, gecerli, True)

    # Bozulmus paket reddedilmeli
    bozuk = bytearray(joystick_paketi(1000, -500, 1))
    bozuk[7] ^= 0x01
    gecerli, _ = paket_coz(bytes(bozuk))
    kontrol("bozuk paket reddedildi", gecerli, False)

    print("\n%d test, %d gecti, %d kaldi" % (gecen + kalan, gecen, kalan))
    return 0 if kalan == 0 else 1


# ---------------------------------------------------------------------------
# Ayristiricinin referans uygulamasi (C tarafiyla ayni algoritma)
# ---------------------------------------------------------------------------

EKSIK, TAMAM, HATALI = "EKSIK", "TAMAM", "HATALI"


class ReferansParser:
    """Kayan aday penceresi: her bayttan sonra tamponun basindan cozmeyi dener."""

    def __init__(self):
        self.sifirla()

    def sifirla(self):
        self.tampon = bytearray()
        self.gecerli = 0
        self.crc_hata = 0
        self.uzunluk_hata = 0
        self.surum_hata = 0
        self.atilan_bayt = 0
        self.paketler = []

    def _coz_dene(self):
        t = self.tampon
        n = len(t)
        if n < 1:
            return EKSIK, 0
        if t[0] != BAS1:
            return HATALI, 0
        if n < 2:
            return EKSIK, 0
        if t[1] != BAS2:
            return HATALI, 0
        if n < 7:
            return EKSIK, 0
        if t[2] != SURUM:
            self.surum_hata += 1
            return HATALI, 0
        if t[4] > MAX_PAYLOAD:
            self.uzunluk_hata += 1
            return HATALI, 0
        toplam = EK_BOYU + t[4]
        if n < toplam:
            return EKSIK, 0
        gelen = t[toplam - 2] | (t[toplam - 1] << 8)
        if gelen != crc16_ccitt(bytes(t[2:toplam - 2])):
            self.crc_hata += 1
            return HATALI, 0
        return TAMAM, toplam

    def besle(self, veri):
        for b in veri:
            self.tampon.append(b)
            while True:
                sonuc, boyu = self._coz_dene()
                if sonuc == EKSIK:
                    break
                if sonuc == TAMAM:
                    self.gecerli += 1
                    t = self.tampon
                    self.paketler.append({
                        "tur": t[3],
                        "sira": t[5] | (t[6] << 8),
                        "uzunluk": t[4],
                        "payload": bytes(t[7:7 + t[4]]),
                    })
                    del self.tampon[:boyu]
                else:
                    del self.tampon[:1]
                    self.atilan_bayt += 1


def _senaryolar():
    """(ad, C dizi adi, parcalar, aciklama) listesi. parcalar: besleme gruplari."""
    jp = joystick_paketi(1000, -500, 1)
    mod = paket_olustur(0x11, 2, bytes([0x01]))
    aa55 = paket_olustur(0x20, 300, bytes([0xAA, 0x55]))

    bozuk_jp = bytearray(jp); bozuk_jp[11] ^= 0x01          # CRC bayti bozuk
    bozuk_aa55 = bytearray(aa55); bozuk_aa55[10] ^= 0x01    # CRC bayti bozuk

    uzunluk_basligi = bytes([BAS1, BAS2, SURUM, 0x20, 56, 0x01, 0x00])
    surum_basligi   = bytes([BAS1, BAS2, 0x02, 0x10, 4, 0x01, 0x00])

    return [
        ("S1  tek tam paket",              "s_tek",       [jp]),
        ("S2  iki parcaya bolunmus",       "s_tek",       [jp[:5], jp[5:]]),
        ("S3  bayt bayt beslenmis",        "s_tek",       [bytes([b]) for b in jp]),
        ("S4  iki paket birlesik",         "s_ikili",     [jp + mod]),
        ("S5  basta cop bayt",             "s_cop",       [bytes([0x00, 0xFF, 0xAA, 0x13]) + jp]),
        ("S6  payload icinde AA 55",       "s_aa55",      [aa55]),
        ("S7  CRC bozuk",                  "s_bozuk",     [bytes(bozuk_jp)]),
        ("S8  bozuk paket + gecerli",      "s_kurtarma",  [bytes(bozuk_aa55) + jp]),
        ("S9  LENGTH 56 + gecerli",        "s_uzunluk",   [uzunluk_basligi + jp]),
        ("S10 VERSION 2 + gecerli",        "s_surum",     [surum_basligi + jp]),
        ("S11 yarim paket + mod paketi",   "s_yarim_mod", [jp[:6] + mod]),
        ("S12 yarim paket, devami yok",    "s_yarim",     [jp[:8]]),
    ]


def komut_akis():
    print("/* ---- Ayristirici test akislari (protokol.py akis ile uretildi) ---- */")
    basilan = set()
    ozet = []
    for ad, dizi_adi, parcalar in _senaryolar():
        tam = b"".join(parcalar)
        if dizi_adi not in basilan:
            print(c_dizisi(dizi_adi, tam))
            basilan.add(dizi_adi)
        rp = ReferansParser()
        for parca in parcalar:
            rp.besle(parca)
        ozet.append((ad, dizi_adi, len(tam), rp))

    print()
    print("/* %-30s %-13s %4s %4s %4s %4s %4s %4s */" %
          ("senaryo", "dizi", "bayt", "gec", "crc", "uzn", "srm", "atl"))
    for ad, dizi_adi, n, rp in ozet:
        print("/* %-30s %-13s %4d %4d %4d %4d %4d %4d */" %
              (ad, dizi_adi, n, rp.gecerli, rp.crc_hata,
               rp.uzunluk_hata, rp.surum_hata, rp.atilan_bayt))
        for pk in rp.paketler:
            print("/*     -> TYPE 0x%02X  SEQ %-5d payload %s */" %
                  (pk["tur"], pk["sira"],
                   " ".join("%02X" % b for b in pk["payload"]) or "(yok)"))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    komut = argv[1]
    if komut == "test":
        return komut_test()
    if komut == "vektor":
        komut_vektor()
        return 0
    if komut == "akis":
        komut_akis()
        return 0
    if komut == "coz":
        if len(argv) < 3:
            print("kullanim: python tools/protokol.py coz \"AA 55 01 ...\"")
            return 1
        veri = hex_coz(" ".join(argv[2:]))
        gecerli, rapor = paket_coz(veri)
        print("\n".join(rapor))
        print("\nSONUC: %s" % ("GECERLI PAKET" if gecerli else "GECERSIZ"))
        return 0 if gecerli else 1
    print("bilinmeyen komut: %s" % komut)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
