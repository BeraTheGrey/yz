# YZ DAC

Apple USB-C → 3.5 mm kulaklık adaptörünü (ve diğer USB Audio Class DAC'ları) Android'de tam performansla kullanmak için bir uygulama. **USB Audio Player PRO (UAPP)** ile aynı mantıkla çalışır: kendi USB ses sürücüsü vardır, DAC'ın donanım ses kontrolüne doğrudan erişir ve isterseniz Android ses sistemini tamamen atlayarak bit-perfect çalar.

Root gerekmez. Minimum Android 8.0 (API 26).

## Ne yapar?

### 1. Sistem geneli mod: donanım sesini maksimuma çekme

Android, USB DAC'ın kendi ses kontrolünü (UAC *Feature Unit*) hiç ayarlamaz. Apple dongle düşük bir varsayılan seviyeyle açıldığı için Android'de olması gerekenden çok daha kısık çalar. Windows, macOS ve iOS bu ayarı yapar.

Bu mod:

1. DAC'ı Android'in kernel sürücüsünden (snd-usb-audio) kısa süreliğine ayırır,
2. çalma yolundaki Feature Unit'i bulur, sesin aralığını okur (`GET_MIN/MAX/RES`, UAC2'de `RANGE`), sesi **cihazın bildirdiği maksimuma** ayarlar ve mute'u kapatır,
3. değeri geri okuyup doğrular, sonra cihazı Android'e geri verir.

Bundan sonra Spotify, YouTube ve diğer tüm uygulamalar tam seviyeden çalar; sesi telefonun ses tuşlarıyla normal şekilde ayarlarsınız.

- **Takınca otomatik uygulama:** Dongle'ı taktığınızda çıkan sistem penceresinde "YZ DAC" ve ardından "Her zaman" seçeneğini seçerseniz, ayar her takışta arayüz açılmadan uygulanır. Ayar dongle çıkarılana kadar geçerlidir.
- **Mikrofon yoluna dokunulmaz:** Topoloji USB-streaming giriş terminalinden fiziksel çıkışa kadar izlenir. Yalnızca o yoldaki Feature Unit'ler değişir.
- **Sürücüyü geri verme yöntemi:** Varsayılan yöntem tüm ses arayüzlerinin sürücüsünü yeniden bağlamaktır. İsterseniz usbDacVolumeAndroid'in kullandığı USB reset yöntemini seçebilirsiniz.

### 2. Exclusive bit-perfect oynatıcı (UAPP modu)

Seçtiğiniz dosyalar Android'in platform codec'leriyle çözülür (FLAC, WAV, ALAC, MP3, AAC, Opus, Vorbis…). Çıkan PCM, libusb üzerinden **isochronous USB aktarımlarıyla doğrudan DAC'a** gönderilir. Bu yolda:

- AudioFlinger mikseri, zorunlu 48 kHz yeniden örnekleme ve 16-bit kesme yoktur.
- Android'in AB "yüksek ses uyarısı" (safe media volume) ve **ses dozu (CSD, EN 50332-3)** sınırlamaları devreye girmez.
- Ses seviyesini DAC'ın **donanım zayıflatıcısı** belirler, bu yüzden dijital sinyal bit-perfect kalır. Telefonun ses tuşları uzak bir `MediaSession` `VolumeProvider` üzerinden DAC'ı kontrol eder, ekran kapalıyken de çalışır.
- Alt ayar (format) otomatik seçilir: kaynakla aynı örnekleme hızı ve en yüksek bit derinliği. DAC kaynak hızını desteklemiyorsa (ör. 96 kHz dosya, 48 kHz dongle) aynı ailedeki en yüksek hıza Kaiser pencereli polifaz sinc dönüştürücüyle çevrilir.
- Asenkron DAC'lar için açık feedback endpoint desteklenir: FS 10.14 ve HS 16.16 biçimleri ile hatalı biçim gönderen cihazlar için Linux benzeri bir sezgisel yöntem kullanılır.
- 24-bit kaynak 16-bit DAC'a giderken TPDF dither uygulanır.
- **Ön kazanç (0…+12 dB, isteğe bağlı):** Sessiz kayıtları yumuşak bir sınırlayıcıyla yükseltir. Açıkken çıkış artık bit-perfect değildir.
- İşitmeyi korumak için ilk çalmada ses DAC maksimumunun 20 dB altından başlar, sonraki çalmalarda son seviye hatırlanır.

Exclusive mod çalarken DAC Android'den ayrıldığı için diğer uygulamaların sesi (bildirimler vb.) telefon hoparlöründen çıkar. Durdurduğunuzda DAC Android'e geri verilir. UAPP'de de durum aynıdır.

## AB (EU) sürümü hakkında: dürüst bilgi

Apple adaptörünün iki donanım sürümü var:

| Model | Bölge | Yaklaşık maks. çıkış |
|---|---|---|
| A2049 | ABD | ~1.0 Vrms |
| A2155 | AB / Hindistan | ~0.5 Vrms |

AB sürümündeki yarım çıkış, AB'nin kişisel müzik çalarlar için koyduğu ses sınırı yüzünden **adaptörün kendi firmware'inde ve analog katında** uygulanır. Bu analog tavanı ne bu uygulama ne de UAPP yükseltebilir. Bu tavanı yazılımla aşabildiğini söyleyen her şey yanıltıcıdır. UAPP de AB sürümünde size 0.5 Vrms'nin tamamını verir, 1 Vrms vermez.

Bu uygulamanın gerçekten kaldırdıkları:

1. Android'in DAC donanım sesini düşük bırakması. Pratikte en büyük kayıp budur ve tamamen düzelir.
2. Android'in AB güvenli ses ve CSD sınırlamaları (exclusive modda).
3. 48 kHz zorunlu yeniden örnekleme ve 16-bit kesme (exclusive modda).
4. **Deneysel:** "Maksimum ötesi testi", cihazın bildirdiği maksimumun üstündeki değerleri 1 dB adımlarla (en fazla +12 dB) dener ve cihazın gerçekten kabul ettiği değeri geri okur. Bazı DAC'lar bildirdiklerinden fazlasını kabul eder; Apple dongle'ın bunu yapması beklenmez. Sonuç ekranda raporlanır ve isterseniz uygulanır.
5. **Ön kazanç:** Kısık masterlanmış kayıtları algıda daha yüksek çalar. Dijital tam ölçeğin (0 dBFS) üstüne çıkamaz.

"Cihaz bilgisi" bölümü cihazın descriptor'larını ve ses aralığını gösterir. ABD ve AB sürümlerinin farklı değer bildirip bildirmediğini karşılaştırmak isterseniz bu çıktıyı paylaşabilirsiniz.

## Güvenlik

Bu uygulama işitme güvenliği için konmuş sınırları kaldırır. Kulaklığı takmadan önce sesi kısın ve hassas IEM'lerde özellikle dikkatli olun. Uzun süre yüksek seste dinlemek kalıcı işitme kaybına yol açar.

## Kurulum

GitHub Actions her push'ta APK üretir: **Actions → Build → yzdac-apk** artifact'ı. `app-release.apk` debug anahtarıyla imzalıdır ve doğrudan kurulabilir.

Kendiniz derlemek için:

```bash
git clone --recursive https://github.com/BeraTheGrey/yz.git
cd yz
./gradlew assembleRelease        # Android SDK + NDK gerekir
```

`--recursive` olmadan klonladıysanız `git submodule update --init` çalıştırın. Bunu da yapmazsanız CMake libusb'yi derleme sırasında kendisi indirir.

Native çekirdeğin host testleri (descriptor parser, simüle edilmiş UAC1/UAC2 DAC'a karşı ses istekleri, bit-perfect dönüşüm, resampler, ring buffer, feedback çözümleme):

```bash
cmake -S native-tests -B build/native-tests && cmake --build build/native-tests && ./build/native-tests/yzdac_tests
```

## Mimari

```
app/src/main/cpp/
  uac/descriptors.*   UAC1/UAC2 descriptor parser (terminaller, FU, clock, alt ayarlar, endpoint'ler)
  uac/control.*       Ses/mute/örnekleme hızı istekleri (UAC1 GET_*/SET_CUR, UAC2 CUR/RANGE)
  dsp/pcm.*           PCM → float → DAC formatı (MSB hizalı alt-slot, TPDF dither, soft clip)
  dsp/resampler.*     Rasyonel adımlı polifaz windowed-sinc SRC
  dsp/ring_buffer.h   Kilitsiz SPSC tampon (decoder ↔ USB event thread)
  engine/iso_stream.* libusb isochronous akış + feedback, URGENT_AUDIO öncelikli event thread
  engine/dac_device.* Kernel sürücüsünü ayırma/geri bağlama, format seçimi, oturum yönetimi
  jni/jni_bridge.cpp  Kotlin köprüsü (JSON sonuçlar)
  libusb/             libusb 1.0.27 (submodule)
app/src/main/java/io/github/berathegrey/yzdac/
  usb/                NativeBridge, DacSession, DacManager (izin, oturum, sistem modu)
  player/             PlayerService (foreground, MediaSession, VolumeProvider), TrackDecoder (MediaCodec)
  ui/                 MainActivity, UsbAttachActivity (takınca otomatik uygulama)
```

AndroidX dahil hiçbir harici Android bağımlılığı kullanılmaz; arayüz yalnızca framework API'leriyle kurulmuştur.

## Teşekkürler

- [100nandoo/usbDacVolumeAndroid](https://github.com/100nandoo/usbDacVolumeAndroid), [guyman624](https://github.com/guyman624/usbDacVolumeAndroid), [polkaulfield](https://github.com/polkaulfield/usbDacVolumeAndroid), [ibaiGorordo/libusbAndroidTest](https://github.com/ibaiGorordo/libusbAndroidTest): Apple dongle'ın Feature Unit 2 üzerinden ses kontrolünü ilk gösteren projeler.
- [libusb](https://libusb.info) (LGPL-2.1).
