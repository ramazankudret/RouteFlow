# RouteFlow UI — Claude Design brief

Bu dosyanın tamamı Claude Design'a yapıştırılmak üzere yazıldı. Teknik
tanımlayıcılar (alan adları, sınıf adları, kanca adları) İngilizce; bunlar koda
birebir karşılık geldiği için çevrilmemeli.

---

## 0. Çıktı formatı — önce bunu oku, en kritik bölüm bu

Bu tasarım gerçek bir C++ sunucunun servis ettiği statik dosyalara dönüşecek.
Tasarımı teslim alan taraf **HTML'i yeniden yazmayacak**: CSS'e ve DOM yapısına
hiç dokunmayacak, sadece işaretlenmiş kancaları canlı veriyle dolduran ince bir
JS katmanı ekleyecek. Bu ancak aşağıdaki sözleşmeye uyulursa mümkün.

**Teknoloji kısıtları — bunlar tercih değil, zorunluluk:**

- Her ekran **tek bir self-contained `.html` dosyası**.
- **Framework yok, CDN yok, build adımı yok.** React, Vue, Tailwind, Alpine,
  jQuery — hiçbiri. Sunucu bağımlılık içermeyen bir C++ HTTP sunucusu; sadece
  düz dosya servis ediyor. Tailwind utility sınıfları özellikle olmaz, çünkü
  çalışması için Tailwind runtime'ı gerekir.
- **Sadece vanilla CSS**, tek bir `<style>` bloğunda, dosyanın en üstünde.
- Tüm renk / boşluk / yarıçap / tipografi değerleri `:root` içinde **CSS custom
  property** olarak tanımlansın. Gövdede ham hex kullanma, `var(--...)` kullan.
- **Harici font yükleme.** Sistem font yığını kullan ya da fontu göm. Router
  localhost'ta ve çevrimdışı çalışabilmeli; `fonts.googleapis.com` çağrısı
  offline'da tasarımı bozar. Karakterli görünmesi gereken yer varsa
  `ui-monospace, "SF Mono", "Cascadia Mono", Menlo, Consolas, monospace` gibi
  gerçek bir yığın ver.
- **İkonlar inline SVG.** Icon font yok, emoji yok.
- JS varsa ayrı bir `<script>` bloğunda ve **sadece etkileşim** için (sekme
  değişimi, satır seçimi, panel aç/kapa). Veri üretmesin, veri çekmesin.

**Yapısal sözleşme — tasarımın hayatta kalmasını sağlayan kısım:**

1. **Tekrarlayan her blok bir `<template>` içinde verilsin.** Node kartı, job
   satırı, candidate satırı, model rozeti gibi. Her `<template>` içinde
   **tam olarak bir** örnek olsun:

   ```html
   <template id="rf-tpl-node-card"> … tek bir node kartının markup'ı … </template>
   ```

   Ayrıca görsel inceleme için sayfada **2-3 tane render edilmiş örnek** dursun.
   Şablon ile render edilmiş örneklerin markup'ı birebir aynı olmalı.

2. **Dinamik her değer `data-rf` özniteliği taşısın.** Değerin adı §2'deki veri
   sözleşmesinden birebir alınacak:

   ```html
   <span class="node-gpu" data-rf="gpu_name">NVIDIA GeForce RTX 4060 Laptop GPU</span>
   <div class="vram-bar-fill" data-rf="vram_used_pct" style="width: 59.4%"></div>
   ```

   Kancalı elemanın içinde **gerçekçi örnek içerik** dursun — boş bırakma,
   `{{placeholder}}` yazma. Örnek içerik hem tasarımı gösterir hem de veri
   gelmezse makul bir görünüm bırakır.

3. **Hesaplanan geometri inline `style` ile.** Bar genişliği, segment yüzdesi,
   nokta konumu — hepsi `style="width: 42.3%"` / `style="left: 61%"` biçiminde,
   üstünde `data-rf` kancasıyla. CSS sınıfına gömme; JS'in yazacağı yer burası.

4. **Durum sınıfları önceden tanımlı ve statik HTML'de görünür olsun.** §4'teki
   her durum için bir sınıf tanımla ve sayfada en az bir örneğini göster:
   `.is-warm`, `.is-cold`, `.is-rejected`, `.is-omitted`, `.is-within-noise`,
   `.is-offline`, `.is-unknown`. Tasarımda görünmeyen bir durumu sonradan icat
   etmek zorunda kalmak, tasarımın bozulmasının bir numaralı sebebi.

5. **DOM yapısı dondurulacak.** Bir elemanı sonradan sarmalamak, bölmek veya
   yerini değiştirmek gerekmesin diye: her mantıksal parçayı kendi kabına koy,
   metin ile kabı karıştırma.

Teslimat: dört ekran için dört `.html` dosyası (§1). Tercihen ortak `:root`
token bloğu dördünde de birebir aynı olsun.

---

## 1. Ne tasarlanacak

RouteFlow, heterojen yerel GPU kümeleri için **sıcaklık farkındalıklı çıkarım
zamanlayıcısı**. Ollama ve LM Studio çalıştıran makineler arasında istek
yönlendiriyor. Bulut yok, API sağlayıcı yok — hepsi yerel donanım.

Ürünün tezi tek cümle: *VRAM'de modeli sıcak tutan zayıf bir makine, modeli
yüklemesi gereken boştaki güçlü makineden daha erken bitirebilir.* Arayüzün işi
bu kararı göstermek **ve gerekçesini savunulabilir kılmak**.

Estetik: koyu, yoğun, **enstrüman paneli**. Referans olarak bir profiler ya da
observability konsolu düşün — tüketici SaaS'ı değil. Sayılar birinci sınıf
vatandaş; telemetri ve süreler tabular rakamlarla hizalı olmalı. Dekoratif
gradyan, yuvarlak köşeli renkli kutucuk, emoji yok.

Dört ekran:

### Ekran A — Cluster view
Küme durumu. Her node için bir kart: GPU adı, VRAM kullanılan/boş barı,
utilization, sıcaklık, güç / güç limiti, engine sağlığı, kaç paralel slotu var.

Kartın **can alıcı kısmı**: her node için **iki ayrı liste** —
`models on disk` ve `models resident in VRAM`. Bu ikisinin farklı kümeler
olduğu bakışta anlaşılmalı; projenin tamamı bu ayrımın üstüne kurulu. Resident
listesindeki her modelin VRAM boyutu ve en son ne zaman kullanıldığı da var.

### Ekran B — Job feed
Gönderilen isteklerin canlı akışı. Satır başına: model, gittiği node, model
**sıcak mıydı soğuk muydu**, TTFT, toplam süre, tahmin edilen süre, sonuç.
Satıra tıklanınca Ekran C açılıyor.

### Ekran C — Decision panel *(kahraman ekran, en çok emeği buraya ver)*
Seçilen job için **değerlendirilen her aday node**, kabul edilen de edilmeyen de.

Kabul edilen her aday için, tahmini tamamlanma süresinin **beş terime ayrılmış
yığılmış yatay çubuğu**. Bütün adaylar **aynı zaman ekseninde** olmalı —
karşılaştırma ancak öyle anlamlı. Terimler:

| terim | ne demek |
|---|---|
| `t_queue` | motorun paralel slotları dolu, sıra bekleme |
| `t_load` | model VRAM'de değil, yüklemek bu kadar sürecek |
| `t_prefill` | prompt'u işleme |
| `t_decode` | token üretme |
| `t_evict` | yer açmak için sıcak bir modeli düşürmenin *sonraki* isteğe maliyeti |

Her çubuğun toplamının ucunda **1-sigma belirsizlik bandı** (whisker). Bu band
dekoratif değil: iki adayın bandları çakışıyorsa karar gürültüden ayırt
edilemiyor demektir ve bunun **görsel olarak** belli olması gerekiyor.

Reddedilen adaylar çubuk almaz — tahmin yapılmadı, sıfır değil **yok**. Ama
satırları durur ve karar anındaki durumlarını (boş VRAM, utilization, inflight)
ve red sebebini gösterir.

Üstte bir **"decided by" rozeti**: kararı hangi terim belirledi. Marj
belirsizlikten küçükse rozet **`within noise`** durumuna geçer — zamanlayıcı
tahmin yürüttüğünü görünür şekilde itiraf etmeli. Bu ekranın en önemli tek
detayı bu olabilir.

Sinyali olmayan terimler **`omitted`** işaretlenir, asla sıfır gösterilmez.

### Ekran D — Prediction accuracy
Tahmin hatası paneli. `predicted_total_ms` ile gerçekleşen `total_ms`in
saçılım grafiği ve 1:1 referans çizgisi. **Ayrıca ve ayrı olarak** çıktı
uzunluğu tahmini hatası (`predicted_output_tokens` vs `output_tokens`) —
bunlar iki farklı sebeple bozulan iki farklı hata, tek sayıda birleştirilmemeli.

---

## 2. Veri sözleşmesi — `data-rf` kanca adları

Bu adlar çalışan koddan alındı; birebir bunları kullan.

### Node (Ekran A)
```
id  gpu_name  telemetry_backend  telemetry_ok
vram_total_bytes  vram_free_bytes  vram_used_pct
gpu_util          temperature_c    power_watts  power_cap_watts
engine            engine_healthy   engine_slots  residency_known
inflight
models_on_disk[]           -> name, disk_bytes
models_resident[]          -> name, vram_bytes, last_used_ms
```

### Job satırı (Ekran B)
```
job_id  model  role_hint  node_id  was_resident
ttft_ms  total_ms  predicted_total_ms  outcome  decided_by
```

### Candidate satırı (Ekran C)
```
node_id  admitted  reason
t_queue  t_load  t_prefill  t_decode  t_evict
predicted_total_ms  sigma_ms  conf  samples
vram_free  gpu_util  was_resident  inflight  would_evict[]
omitted[]
```
Karar başlığı: `decided_by`, `margin_ms`.

`conf` üç değer alır: `seeded` | `learning` | `converged` — modelin bu tahmini
ne kadar veriye dayandırdığı. Rozet olarak göster.

### Ekran D'nin türetilmiş kancaları

Ekran D'deki sayıların çoğu bir trace alanı değil; `rf-bind.js` bunları aynı
iş kümesinden hesaplıyor. Tasarım revize edilirse bu adlar korunmalı,
çünkü karşılıkları trace'te aranarak bulunamaz:

```
timing_all_value   timing_all_note
timing_warm_label  timing_warm_value  timing_warm_note
timing_cold_label  timing_cold_value  timing_cold_note
timing_worst_value timing_worst_note

length_all_value   length_all_note
length_over_value  length_over_note
length_early_value length_early_note
length_worst_value length_worst_note

dist_n  dist_count  dist_warm  dist_cold  dist_summary
        + tekrarlanan blok: rf-tpl-bucket, rf-tpl-bucket-label

node_axis  warm_err  warm_n  cold_err  cold_n  node_note
        + tekrarlanan blok: rf-tpl-node-error

drift_steps  drift_axis  drift_time  drift_note  drift_summary
drift_event  drift_event_label  cold_band
```

**Geometri kancayla yazılmaz.** Histogram çubukları (`.h-warm` / `.h-cold`),
dumbbell noktaları (`.dpt`) ve saçılım noktaları sınıfla seçilir. Aynı kanca
adı sayfada hem metin hem geometri taşıyabiliyor; `setHook` belge genelinde
yazıyor ve bu, bir kez sayıları çubukların içine doldurmuştu.

**Boş pencere bir durumdur, bir istisna değil.** Ekran D'nin her paneli
kendi boşluğunu söylemek zorunda — hiç iş yokken fikstür sayılarının
kalması, hiç çalışmamış bir zamanlayıcı için eksiksiz bir ölçüm seti
iddia etmek demek.

---

## 3. Gerçek örnek veri

Aşağıdaki sayılar bu makinede gerçekten ölçüldü; uydurma değil. Tasarımda
bunları kullan.

**Node `desktop-01`** — NVIDIA GeForce RTX 4060 Laptop GPU
`vram_total_bytes` 8 585 740 288 · `vram_free_bytes` 3 484 819 456 ·
`gpu_util` 0.07 · `temperature_c` 52 · `power_watts` 15.7 / `power_cap_watts` 100 ·
engine `ollama`, healthy, `engine_slots` 1 · `telemetry_backend` `nvml`
on disk: `qwen2.5:7b-instruct-q4_K_M` (4 683 087 332 B)
resident: `qwen2.5:7b-instruct-q4_K_M` (4 748 056 984 B)
*Ölçülen soğuk yükleme süresi: ~9.5 s.*

**Node `jetson-01`** — Jetson Orin NX (unified memory, 16 GB, yavaş ama sıcak)
`telemetry_backend` `tegra`. Güç limiti düşük, sıcaklığı yüksek olsun.

**Node `laptop-01`** — 4 GB'lık zayıf makine. Model sığmıyor; reddedilen aday
örneği olarak kullanılacak. `telemetry_backend` `null`, yani `telemetry_ok`
false → utilization / sıcaklık / güç **`—`** gösterilmeli, `0` değil.

### Ekran C için üç örnek job — üçü de farklı bir durumu göstersin

**Job 1 — tezin kendisi.** `qwen2.5:7b-instruct-q4_K_M`, 1842 prompt token,
`role_hint` `planner`.

| aday | t_queue | t_load | t_prefill | t_decode | t_evict | toplam | σ |
|---|---|---|---|---|---|---|---|
| `jetson-01` **kazanan**, sıcak | 0 | 0 | 620 | 8200 | 0 | **8820** | 2400 |
| `desktop-01` boşta ama soğuk | 0 | 9500 | 150 | 3400 | 0 | 13050 | 1900 |
| `laptop-01` | — | — | — | — | — | reddedildi | — |

`laptop-01` reddi: `insufficient_vram`.
`margin_ms` 4230, en büyük σ 2400 → 4230 > 2400, yani karar gürültünün üstünde.
`decided_by` = **`t_load`**.

Bu tablonun anlattığı şey ürünün varlık sebebi: *boştaki güçlü makine, sıcak
zayıf makineye kaybediyor.* Tasarım bunu bakışta okutmalı.

**Job 2 — `within noise` durumu.** Küçük model, kısa prompt.
`desktop-01` toplam 1850 (σ 520) · `jetson-01` toplam 2100 (σ 610).
Marj 250, σ 610'dan küçük → `decided_by` = **`within_noise`**. Rozet farklı
görünmeli; zamanlayıcı burada seçim yapıyor ama ayırt edemediğini söylüyor.

**Job 3 — `t_evict` görünür olsun.** `desktop-01`'de yer açmak için 40 saniye
önce kullanılmış sıcak bir model düşürülüyor: `would_evict` dolu,
`t_evict` sıfırdan büyük.

---

## 4. Tasarlanması zorunlu durumlar

Bunlar "sonra hallederiz" değil; her biri gerçek ve her biri tasarımda
görünmezse sonradan uydurulmak zorunda kalınır.

**Node kartı:**
- `engine_healthy` false → node çevrimdışı görünümü
- `telemetry_ok` false → utilization / sıcaklık / güç `—`, sıfır değil
- `residency_known` false → resident listesi yerine "residency unknown"; bu
  node için `t_load` hesaplanamıyor demek
- resident listesi boş (her şey soğuk)
- `inflight` > `engine_slots` → kuyruk var

**Candidate satırı — beş red sebebi, beşi de tasarlansın:**
`insufficient_vram` · `model_missing` · `engine_down` · `node_stale` · `excluded`

**Karar rozeti:** `t_queue` / `t_load` / `t_prefill` / `t_decode` / `t_evict` /
`within_noise` / `single_candidate` *(tek aday vardı, kıyas yok)*

**Job sonucu:** `ok` · `no_candidate` · `dispatch_failed` · `stream_failed` ·
`client_abort` · `timeout`

**Boş durumlar:** hiç node yok · hiç job yok · hiç trace yok (accuracy ekranı)

---

## 5. Grafik renkleri — bunlar sabit, değiştirme

Beş terimin rengi renk körlüğü için doğrulandı (OKLab ΔE, koyu yüzey
`#171a1f` üzerinde; en kötü komşu çift ΔE 8.4 protan, normal görüş 19.3 —
hepsi geçti). Bu beşi **verilen sırayla** kullan:

| terim | hex |
|---|---|
| `t_queue` | `#3987e5` |
| `t_load` | `#d95926` |
| `t_prefill` | `#199e70` |
| `t_decode` | `#c98500` |
| `t_evict` | `#d55181` |

`t_load`'un turuncu olması kasıtlı: soğuk başlatma maliyeti pahalı olan şey ve
tema zaten "sıcaklık". Ekran D'deki saçılımda da aynı anlam korunsun — soğuk
başlangıçlı işler `#d95926`, sıcak olanlar `#3987e5`.

Grafik kuralları:
- Yığılmış segmentler arasında **2px yüzey rengi boşluk** (kenarlık çizme).
- Izgara ve eksen çizgileri **düz hairline**, kesikli değil.
- İki veya daha fazla seri varsa **legend her zaman var**; renk tek başına
  kimlik taşımasın.
- Her değere etiket yazma; sadece uç / kritik olanları doğrudan etiketle.
- Çift y-ekseni yok.

Yüzey, tipografi, chrome ve genel estetik sana ait — yukarıdaki beş renk ve bu
kurallar dışında serbestsin.

---

## 6. Ölçek

Ekranlar masaüstü: 1440×900 çalışma genişliği. Bu bir operatör konsolu, mobil
öncelikli değil; ama yatay taşma olmasın, geniş tablolar kendi içinde kaysın.
