<!-- translated from README.md 84c6832c04b6 body ec7b7c06d5e9 -->

<p align="center"><img src="../images/icon.svg" width="144" alt="Mario Kart World: Pit Stop icon"></p>

<h1 align="center">Mario Kart World: Pit Stop</h1>

<p align="center">
  <b>منفذ homebrew أصلي لـ PlayStation 5 للعبة Mario Kart World (Nintendo Switch 2)، بإعادة تجميع ثابتة.</b>
</p>

<p align="center">
  <a href="https://github.com/exploitizm/mkw-pit-stop/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_Download-latest_for_PS5-0070d1?style=for-the-badge&labelColor=1b2740" alt="Download: latest for PS5"></a>
</p>

<p align="center">
  <b><a href="https://github.com/exploitizm/mkw-pit-stop/releases">الإصدارات</a></b> ·
  <a href="../../CHANGELOG.md">سجل التغييرات</a> ·
  <a href="../../CONTRIBUTING.md">المساهمة</a> ·
  <a href="../../docs/project/roadmap.md">خارطة الطريق</a> ·
  <a href="../../docs/ps5/ps5.md">ملاحظات PS5</a> ·
  <a href="https://github.com/exploitizm/mkw-pit-stop/issues">المشكلات</a>
</p>

<!-- i18n-bar:start -->
<p align="center">
  <a href="../../README.md">English</a> ·
  <a href="README.zh-CN.md">简体中文</a> ·
  <a href="README.es.md">Español</a> ·
  <b>العربية</b> ·
  <a href="README.hi.md">हिन्दी</a> ·
  <a href="README.pt-BR.md">Português (Brasil)</a> ·
  <a href="README.pt-PT.md">Português (Portugal)</a> ·
  <a href="README.de.md">Deutsch</a>
</p>
<!-- i18n-bar:end -->

<div dir="rtl">

منفذ homebrew أصلي لـ PlayStation 5 (`PPSA99193`) من [**mkw-recomp**](https://github.com/adriadam10/mkw-recomp)، وهو
إعادة التجميع الثابتة التي أعدّها Adrià للعبة *Mario Kart World* (Nintendo Switch 2، الإصداران 1.8.0 و1.0.0). تحتاج إلى نسختك
الخاصة من اللعبة، مستخرجة من جهاز Switch 2 الخاص بك: لا يوجد أي شيء من اللعبة في هذا المستودع ولا في إصداراته.

**إنها إعادة تجميع ثابتة، وليست محاكيًا.** تُترجم شيفرة AArch64 الخاصة باللعبة إلى C مسبقًا وتُجمَّع لمعالج x86-64 Zen 2 في
PS5 (تخطيط عناوين الضيف `ps5-high144`)؛ لا يوجد مفسّر للمعالج ولا JIT. ولا تتم محاكاة وحدة الرسوميات أيضًا: أُعيد تنفيذ NVN2،
واجهة الرسوميات في Switch 2، على مستوى الواجهة فوق Vulkan باستخدام برنامج تشغيل RADV من Mesa لـ PS5، وتُترجم مظلّلات SM86
الخاصة باللعبة إلى SPIR-V مسبقًا على حاسوبك. وتُستبدل دوال Nintendo SDK (`nn::*`) بأسمائها بشيفرتنا الخاصة.

> **مشروع الحاسوب الأصلي (upstream):** تبحث عن نسخة Linux أو Windows؟ راجع
> [**adriadam10/mkw-recomp**](https://github.com/adriadam10/mkw-recomp) ([تعليمات الحاسوب أدناه](#اللعب-على-الحاسوب-linux-وwindows)).

## الحالة (PS5)

في مرحلة مبكرة، ويتقدّم بسرعة. حتى 18/11/2026 على جهاز PS5 أساسي:

- **الإقلاع والمقاطع والقوائم والسباقات تعمل**، مع عرض Vulkan مباشر عبر RADV، وصوت ستيريو عبر `sceAudioOut`، وما يصل إلى أربع
  وحدات تحكم DualSense بحسابات مسجّلة، مع الاهتزاز وأيقونات أزرار PlayStation.
- **تعمل السباقات بمعدل 60 إطارًا في الثانية افتراضيًا، دون توليد إطارات**: نحو 57-59 إطارًا في الثانية في المتوسط في اختبار
  أداء للجائزة الكبرى، وبعض الإطارات الأبطأ أحيانًا تُبقي السباق عند 0.95-0.98 من الزمن الحقيقي (انظر [الأداء](#الأداء)).
  وضع 30 إطارًا في الثانية الخاص باللعبة (بالسرعة الكاملة) وتوليد الإطارات خياران في **Pit Stop > Display**.
- **اللعب الجماعي:** الشاشة المقسومة الخاصة باللعبة (وحدة DualSense لكل لاعب مسجّل) ووضعها اللاسلكي المحلي عبر الشبكة المحلية،
  بين أجهزة PS5 أو مع نسخة الحاسوب من الإصدار نفسه ([docs/ps5/multiplayer.md](../../docs/ps5/multiplayer.md)). لا يوجد لعب عبر
  الإنترنت من Nintendo.
- **واجهة Pit Stop داخل اللعبة.** اضغط **Options + نقرة لوحة اللمس** في أي وقت لفتح **Pit Stop**، وهي قائمة إعدادات مصمّمة
  لوحدة التحكم، فيها صفوف سريعة (**Master volume** و**Vibration strength** و**Performance HUD**)، وكل فئات الإعدادات، وإعادة
  تعيين أزرار DualSense مباشرةً، وتُحفظ في `/app0/mkw.ini`.
- **مشكلات معروفة ([docs/project/roadmap.md](../../docs/project/roadmap.md)):**
  - *الجلسات الطويلة:* تنخفض الذاكرة المتاحة سباقًا بعد سباق لأن اللعبة تحتفظ ببيانات الحلبات السابقة؛ وعندما تقلّ، قد تظهر
    الخامات تالفة أو مفقودة. أعد تشغيل اللعبة بعد بضعة سباقات إذا حدث ذلك.
  - *البلاطات المتفرّقة:* منذ الإقلاع تبقى بعض بلاطات الخامات المتفرّقة غير محمّلة: عيوب طفيفة في الخامات قرب خط البداية ومقدمة
    سباق بالأبيض والأسود.
  - *اللعب الجماعي:* قد يُسقط السباق الأول على حلبة لاعبًا ما زالت مظلّلاته قيد التجميع؛ والسباقات التالية تعمل. تعمل الشاشة
    المقسومة بأقل من 30 إطارًا في الثانية في المشاهد المزدحمة.
  - *القوائم:* ما زالت شاشات اختيار الوضع والشخصية والمركبة تختلف قليلًا عن الجهاز الأصلي.

## ما تحتاجه

1. **نسختك المستخرجة الخاصة من *Mario Kart World*** من جهاز Nintendo Switch 2 الخاص بك: الإصدار **1.8.0** (`exefs/main`،
   معرّف البناء `d00e4f399e411d06`) أو 1.0.0 (`9e244438c3d8ef1c`)، على شكل مجلدَي `exefs/` و`romfs/` مفكوكَي التشفير أو ملف
   `.nup`. تُستخدم النسخة المستخرجة من الجهاز التي تحتوي على تحديث 1.8.0 في `updates/` كما هي، وإذا وُجد الاثنان فيُبنى 1.8.0.
2. **جهاز PS5 مكسور الحماية** يشغّل محمّل حمولات homebrew (`etaHEN` أو `elfldr`) و`kstuff` و**ShadowMountPlus 1.7 Beta 5**
   أو أحدث (يركّب `/data/homebrew/PPSA99193` ويسجّله على الشاشة الرئيسية). يعتمد دعم البرنامج الثابت على كسر الحماية لديك وعلى
   `kstuff` وبيئة RADV الخاصة بالـ homebrew (انظر [docs/ps5/ps5.md](../../docs/ps5/ps5.md)).
3. **حاسوب فيه نحو 30 غيغابايت من المساحة الحرة** على الشبكة نفسها التي عليها PS5، لبناء اللعبة من نسختك المستخرجة:
   - **Windows 10 أو 11 (64 بت):** لا حاجة إلى تثبيت أي شيء آخر، ولا WSL. يحمل مُنشئ Windows نسخته الخاصة من Python وzig وglslc
     ([خطوات Windows](#التثبيت-على-windows-دون-wsl)).
   - **Linux x86-64 أو WSL2:** `clang` و`lld` وPython 3 ([خطوات Linux](#التثبيت-والتحديث-على-ps5-linux-أو-wsl2)).

## التثبيت على Windows (دون WSL)

1. **ضع اللعبة المؤقتة على PS5.** فكّ ضغط `mkw-pit-stop-<version>-ps5-placeholder-PPSA99193.zip` من
   [**الإصدارات**](https://github.com/exploitizm/mkw-pit-stop/releases) وارفع مجلد `PPSA99193` الموجود فيه إلى `/data/homebrew/`
   ببرنامج FTP (FileZilla أو WinSCP)، بحيث يوجد `/data/homebrew/PPSA99193/eboot.bin`. بعد أن يسجّلها ShadowMountPlus، شغّل
   **Mario Kart World: Pit Stop**: ستعرض **Game files missing** و`Console IP: <العنوان> (port 19913)`. اترك تلك الشاشة مفتوحة؛ لا
   يستقبل PS5 الملفات إلا أثناء عرضها.
   <p align="center"><img src="../../docs/images/placeholder-ps5.png" width="560" alt="اللعبة المؤقتة على PS5: ملفات اللعبة مفقودة، وعنوان الجهاز، وخطوة البناء على الحاسوب وتقدّمه"></p>
2. **فكّ ضغط المُنشئ على الحاسوب.** نزّل `mkw-pit-stop-<version>-ps5-builder-windows-x86_64.zip`، وانقر عليه بالزر الأيمن، ثم
   **Properties**، وضع علامة على **Unblock**، ثم استخرجه إلى مجلد قصير الاسم على قرص فيه 30 غيغابايت حرة، مثل `C:\PitStop` (وليس
   `Program Files` أو OneDrive).
3. **ابنِ وأرسل.** شغّل `mkw.exe` من ذلك المجلد. في **Game files**، اختر نسختك المستخرجة بـ **Choose folder** (أو
   **Choose .nup or .nx file**)؛ وتحت **Build for PS5**، اكتب العنوان الظاهر على شاشة PS5 في **PS5 IP** واضغط **Build for PS5**. لا
   حاجة إلى زر **Set up** الخاص بنسخة الحاسوب من أجل PS5. أو من موجّه الأوامر في ذلك المجلد:
   ```bat
   prepare_ps5.bat --source "D:\Dumps\MKW" --send 192.168.1.50
   ```
   <p align="center"><img src="../../docs/images/launcher-ps5.png" width="560" alt="شاشة بدء مُنشئ PS5: نسختك مختارة، وعنوان PS5 مكتوب، وزر Build for PS5"></p>
4. **ابدأ اللعبة.** أثناء بناء الحاسوب، تعرض شاشة PS5 الخطوة والتقدّم؛ وتعرض **Transfer complete!** عندما تصل كل الملفات ويتم
   التحقق منها: اضغط **X**، ثم شغّل **Mario Kart World: Pit Stop** مرة أخرى.

**نسختك موجودة على PS5 بالفعل؟** ضع علامة على **My game files are already on the PS5** واكتب مجلدها هناك (مثلًا `/data/mkw`،
وفيه أو في `updates\<version>` الخاص به المجلدان `exefs` و`romfs`): تُرسل شيفرة اللعبة والمظلّلات فقط، وتجد اللعبة نفسها في
ذلك المجلد. وما زال الحاسوب يحتاج إلى نسختك المستخرجة للبناء.

**لا توجد شبكة؟ استخدم قرص USB.** في الخطوة 3، اختر **To a USB drive** و**Choose drive** بدلًا من كتابة عنوان PS5: تُكتب
اللعبة في `homebrew\PPSA99193` على القرص (**exFAT**، نحو 25 غيغابايت حرة)، ويجدها ShadowMountPlus هناك بمجرد توصيل القرص بـ PS5
(فهو يبحث في `/mnt/usb0`-`usb7/homebrew/`). لا حاجة إلى اللعبة المؤقتة من الخطوة 1 لهذا؛ وإذا كانت مثبّتة، فاحذف
`/data/homebrew/PPSA99193` أولًا حتى تُسجَّل نسخة واحدة فقط من اللعبة. وإعادة البناء تحدّث فقط ما تغيّر على القرص.

إذا بقي PS5 على **Waiting for connection**، فتحقّق من العنوان ومن أن الجهازين على الشبكة نفسها؛ ويُشرح سبب فشل البناء في
`logs/setup.log` بجانب `mkw.exe`. للتحديث، فكّ ضغط المُنشئ الجديد وابنِ مرة أخرى.

## التثبيت والتحديث على PS5 (Linux أو WSL2)

لأن `eboot.bin` يربط شيفرة اللعبة المعاد تجميعها ويحتوي `spirv/` على مظلّلات اللعبة المترجمة، **لا يتم توزيع أي `eboot.bin` أو
`spirv/` جاهز للتشغيل**. تبني مجلد اللعبة `PPSA99193` على حاسوبك من مُنشئ الإصدار الخالي من اللعبة ومن نسختك المستخرجة:

1. نزّل `mkw-pit-stop-<version>-ps5-builder-linux-x86_64.tar.zst` من
   [**الإصدارات**](https://github.com/exploitizm/mkw-pit-stop/releases)، وطابق بصمته مع `SHA256SUMS`، وفكّ ضغطه على Linux أو WSL2:
   ```sh
   tar --zstd -xf mkw-pit-stop-*-ps5-builder-linux-x86_64.tar.zst
   cd mkw-pit-stop-*
   ```
2. شغّل `prepare_ps5.py` موجّهًا إلى نسختك المستخرجة من *Mario Kart World* (مجلد أو ملف `.nup`):
   ```sh
   python3 prepare_ps5.py --source /path/to/your/mkw-dump --out ./PPSA99193
   ```
   يتعرّف السكربت على إصدار نسختك (1.8.0 أو 1.0.0)، ويترجم ExeFS الخاص بـ AArch64 إلى C (`ps5-high144`)، ويجمّع `eboot.bin`
   ويربطه بوقت التشغيل المبني مسبقًا وأرشيف RADV الخاص بـ PS5، وينزّل `nvdisasm` و`ptxas` من NVIDIA (مثبّتين ببصمة SHA-256)
   لتوليد مواصفات تعليمات SM86 محليًا، ويترجم كل المظلّلات البالغ عددها 11,561 إلى `PPSA99193/spirv/`، ويضع `exefs/` و`romfs/` في
   `PPSA99193/`.
3. انقل `PPSA99193` إلى `/data/homebrew/PPSA99193` على جهاز PS5 عبر FTP أو USB — أو مرّر `--deploy <PS5_IP>` للرفع، والتحقق من كل
   ملف ببصمة SHA-256، وتحديث `param.json` و`icon0.png` اللذين سجّلهما ShadowMountPlus في `/user/app/PPSA99193/sce_sys`.
4. شغّل **Mario Kart World: Pit Stop** من الشاشة الرئيسية لـ PS5.

**التحديث:** فكّ ضغط المُنشئ الأحدث وشغّل `prepare_ps5.py` بمجلدَي `--work` و`--out` نفسيهما. تُتخطّى الخطوات التي لم تتغيّر
مدخلاتها (مثل `romfs/` والمظلّلات غير المتغيّرة)، وتبقى ملفات الحفظ (`profile/`) والإعدادات (`mkw.ini`) على الجهاز كما هي.

## واجهة إعدادات Pit Stop

اضغط **Options + نقرة لوحة اللمس** على DualSense (أو **F1** على الحاسوب) من أي شاشة لفتح **Pit Stop**
([docs/ps5/pit-stop.md](../../docs/ps5/pit-stop.md)):

| التحكم | الإجراء |
|---|---|
| **Options + نقرة لوحة اللمس** | فتح Pit Stop أو إغلاقه من أي شاشة |
| **أزرار الاتجاه / العصا اليسرى أعلى وأسفل** | الانتقال صفًا واحدًا |
| **أزرار الاتجاه / العصا اليسرى يمين ويسار** | تعديل الإعداد المحدد (إيقاف/تشغيل أو قيمة متدرّجة) |
| **الدائرة / X** | تأكيد / رجوع (يتبع تخطيط أزرار Nintendo أو Xbox لديك) |
| **L1 / R1** | الفئة السابقة / التالية داخل **All settings** |

- **الصفوف السريعة** في الشاشة الأولى تضبط مباشرةً **Master volume** (0–100%) و**Vibration strength** (0–300%) و
  **Performance HUD** (Off وFPS وFull).
- **All settings** تفتح **Display** و**Graphics** و**Controls** (مع التقاط الأزرار لكل إجراء مباشرةً) و**Audio** و**General**
  و**Advanced** و**Hacks** (فتح كل السائقين والمركبات والملصقات والكأس الخاصة) و**Multiplayer**. تُحفظ التغييرات تلقائيًا في
  `/app0/mkw.ini`.

## اللعب على الحاسوب (Linux وWindows)

من المشروع الأصلي [mkw-recomp](https://github.com/adriadam10/mkw-recomp):

<p align="center">
  <img src="../../docs/images/launcher-files.png" width="400" alt="صفحة Game files في شاشة البدء قبل الإعداد">
  <img src="../../docs/images/launcher-main.png" width="400" alt="شاشة البدء بعد إعداد اللعبة">
</p>

1. استخرج Mario Kart World 1.0.0 أو 1.8.0 من جهاز Switch 2 الخاص بك (`exefs/` و`romfs/`، مفكوكَي التشفير).
2. نزّل مشغّل الحاسوب من [إصدارات mkw-recomp](https://github.com/adriadam10/mkw-recomp/releases): على Linux ملف `.AppImage` (يحفظ
   اللعبة التي يبنيها في `~/.local/share/mkw-recomp`؛ glibc 2.31 أو أحدث)، وعلى Windows ملف `.zip`. يحتاج كلاهما إلى نحو 30
   غيغابايت حرة.
3. شغّل AppImage أو `mkw.exe`، وافتح **Game files**، ووجّهه إلى نسختك المستخرجة واضغط **Prepare**، ثم **Play**.

## البناء من الشيفرة المصدرية

### هدف PS5 الأصلي (`PPSA99193`)

راجع [docs/ps5/ps5.md](../../docs/ps5/ps5.md) و[tools/ps5/README.md](../../tools/ps5/README.md) للنسخ المثبّتة من `PS5_VulkanTemplate`
و`PS5_Vulkan` و`PS5_Mesa` و`PS5_PayloadSDK`:

```sh
python3 recomp/gen.py --exefs "$MKW_GAME/exefs" --guest-layout ps5-high144 \
    --out build/ps5-game/generated --patches recomp/patches-mkw.txt
python3 tools/shaders/extract.py && python3 tools/shaders/translate.py
python3 tools/ps5/build_game.py --stack "$stack" --include "$headers" \
    --generated build/ps5-game/generated --exefs "$MKW_GAME/exefs"
python3 tools/ps5/deploy.py <PS5_IP> --game
```

لحزم إصدار مُنشئ PS5 خالٍ من اللعبة من جرد وقت التشغيل (`--inventory`):

```sh
python3 tools/ps5/build_game.py --stack "$stack" --include "$headers" --inventory
python3 tools/ps5/release_ps5.py --stack "$stack" --version 0.1.0
```

### البناء لـ Linux وWindows

التفاصيل في [tools/build/README.md](../../tools/build/README.md):

```sh
python3 recomp/gen.py --exefs game/exefs --out build/generated --jobs 8 --patches recomp/patches-mkw.txt
python3 tools/shaders/extract.py && python3 tools/shaders/translate.py
python3 tools/build/build.py              # build/mkw; --target windows builds mkw.exe with llvm-mingw
build/mkw
```

| المجلد | المحتوى |
|---|---|
| [recomp/](../../recomp/README.md) | المولّد: AArch64 → C |
| [runtime/](../../runtime/README.md) | ما يقوم مقام الجهاز: النواة والذاكرة وبدائل SDK وNVN2 فوق Vulkan والصوت والإدخال وPit Stop والمشغّل |
| [sm86/](../../sm86/README.md) | مترجم المظلّلات: SM86 SASS → GLSL → SPIR-V |
| [tools/](../../tools/README.md) | التحليل والإعداد والبناء وحزم PS5 وتشغيل اللعبة والتشخيص |
| [tests/](../../tests/README.md) | اختبارات دون اللعبة أو وحدة رسوميات، في التكامل المستمر على Linux وWindows |
| [docs/](../../docs/README.md) | بنية PS5 وPit Stop والبحث والمصادر وخارطة الطريق والأداء |
| [i18n/](../../i18n/README.md) | نصوص القائمة كملفات gettext (`mkw.pot`، وملف `.po` لكل لغة) |
| [server/](../../server/README.md) | خادم الردهة للعب اللاسلكي المحلي عبر الإنترنت (`server/lobby`) |
| `.github/` | التكامل المستمر وقوالب المشكلات وطلبات الدمج |

في الجذر: `pyproject.toml` و`uv.lock` (بيئة Python للأدوات)، و`Makefile` (اختصارات لمسارات الإصدار والتكامل المستمر)،
و`.gitignore` و`LICENSE` و`CONTRIBUTING.md` و`CHANGELOG.md` (تغييرات كل إصدار وملاحظاته) و`AGENTS.md` (دليل لوكلاء البرمجة).

## الأداء

على جهاز PS5 أساسي، Mario Kart World 1.8.0، اختبار أداء لسباق الجائزة الكبرى (`tools/ps5/bench.py`، 210 ثوانٍ من السباق،
1080p، توليد الإطارات متوقف):

| التاريخ | إعداد معدل الإطارات | متوسط زمن الإطار | الإطار المعتاد | أدنى 1% | سرعة السباق |
|---|---|---|---|---|---|
| 20/11/2026 | 60 إطارًا/ث (الافتراضي الجديد) | 17.0–17.4 مللي ثانية (57–59 إطارًا/ث) | 16.7 مللي ثانية (عند الحد) | 35–41 إطارًا/ث | 0.95–0.98x |
| 16/11/2026 | 30 إطارًا/ث (الافتراضي السابق) | 33.3 مللي ثانية | 33.3 مللي ثانية | | 1.00x |

لم يُختبر الافتراضي الجديد على الجهاز كما يُشحن: تأتي هذه الأرقام من سباقات اختبار أداء مع تشغيل سجل الأداء الخاص بوقت التشغيل
(`MKW_TRACE=perf`، وله كلفة بسيطة)، مضبوطة على 60 إطارًا في الثانية يدويًا. لم تُلعب القوائم وسباقات الوقت والشاشة المقسومة
والجلسات الطويلة والحلبات الأخرى بالافتراضي الجديد بعد.

نحو 2% من إطارات السباق تستغرق أكثر من 20 مللي ثانية: وهي حيث تحمّل الحلبة خامات جديدة، وهي التي تحدد أدنى 1% والتباطؤ
الطفيف. ما قيس وما بقي موجود في [docs/project/roadmap.md](../../docs/project/roadmap.md) و[docs/ps5/ps5.md](../../docs/ps5/ps5.md#frame-rate)؛
وأرقام الحاسوب في [docs/project/performance.md](../../docs/project/performance.md).

## الإبلاغ عن الأخطاء

راجع [CONTRIBUTING.md](../../CONTRIBUTING.md). أبلغ عن مشكلات PS5 في [مشكلات هذا الفرع](https://github.com/exploitizm/mkw-pit-stop/issues)
وأرفق سجل نواة الجهاز (`klog`، بالبادئة `[Mario Kart World: Pit Stop]`) و`logs/setup.log` من مُنشئ الحاسوب. ولمشكلات نسخة
الحاسوب، أرفق `logs/mkw.log` و`logs/setup.log`. لا ترفق أبدًا ملفات اللعبة أو النسخ المستخرجة أو ملفات `eboot.bin` المبنية من
نسختك.

## التراخيص والشكر وإخلاء المسؤولية

الشيفرة المصدرية لهذا المستودع مرخّصة بـ [رخصة MIT](../../LICENSE) (`Copyright (c) 2026 Adrià`).

**ملفات إصدارات PS5 مرخّصة بـ GPL-3.0-or-later.** كل لعبة PS5 تربط طبقة المنصة من فرع SDK الحمولات وشيفرة الإقلاع من
`PS5_Vulkan` (GPL-3.0-or-later)، لذا فإن اللعبة المؤقتة و`ps5-bundle/` الخاص بالمُنشئ وكل لعبة تبنيها بهما مرخّصة ككل بـ
GPL-3.0-or-later؛ وتبقى شيفرة mkw-recomp الخاصة بـ MIT. يحمل كل إصدار `LEGAL.txt` و`licenses/` (كل جزء مع رخصته وحقوق نشره
ومراجعة مصدره) ويرفق الشيفرة المصدرية الكاملة لكل جزء على شكل `source/*.tar.gz` (`source/SOURCES.txt`)، يولّدها
[tools/ps5/release_notices.py](../../tools/ps5/release_notices.py).

- **المشروع الأصلي:** [**mkw-recomp**](https://github.com/adriadam10/mkw-recomp) لـ Adrià — رافع AArch64→C، ونواة Switch 2
  وبدائل `nn::*` SDK، ومُصيّر NVN2 فوق Vulkan، ومترجم المظلّلات SM86→SPIR-V دون اتصال.
- **مشروعات MIT والمشروعات المتساهلة** (ملفات `LICENSE.*` في [recomp/](../../recomp/) و[runtime/](../../runtime/) و
  [sm86/decode/](../../sm86/decode/) و[runtime/third_party/](../../runtime/third_party/README.md)):
  [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) ومنفذه لـ Linux،
  [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp)،
  [DocumentSASS](https://github.com/0xD0GF00D/DocumentSASS) و
  [nvsass-disassembler](https://github.com/reoLantern/nvsass-disassembler)،
  [Ryujinx](https://git.ryujinx.app/ryubing/ryujinx)، [Mesa NAK](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/nouveau/compiler)،
  [mario-kart-world-save-editor](https://github.com/Ca1nM/mario-kart-world-save-editor)، وDear ImGui،
  [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo)، وAMD FidelityFX FSR 1، وVulkan Memory Allocator، وRoboto وLato.
- **سلسلة أدوات PS5 ووقت التشغيل:** [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate) (MIT، Sascha Willems
  وMihawk)، و[PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) (MIT، RADV لـ PS5)،
  و[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) و[PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK)
  (GPL-3.0-or-later، Mihawk و[John Törnblom / ps5-payload-dev](https://github.com/ps5-payload-dev/sdk))، و
  [ShadowMountPlus](https://github.com/drakmor/shadowMountPlus). عند ربطها في لعبة PS5 مع طبقة منصة SDK الحمولات وCRT/الدوال
  البديلة من `PS5_Vulkan`، يكون الملف الثنائي المجمّع للعبة PS5 مرخّصًا بـ GPL-3.0-or-later (`LEGAL.txt` و`licenses/` داخل اللعبة
  المبنية).
- **أفكار بحث وتصميم فقط** (لم تُنسخ شيفرة من مشروعات GPL أو غير مرخّصة): eden-nx2 وHookshot وPS5_Hookshot وEglantinePS5
  وAnyPS5 وprosper وshadPS4. كل مصدر موثّق في [docs/project/research.md](../../docs/project/research.md#sources).

**إخلاء المسؤولية:** *Mario Kart World* و*Mario* علامتان تجاريتان لـ Nintendo. و"PlayStation" و"PS5" علامتان تجاريتان لـ Sony
Interactive Entertainment Inc. هذا المشروع غير تابع لـ Nintendo أو Sony Interactive Entertainment أو أي ناشر ألعاب ولا يحظى
بتأييدها، ولا يحتوي على أي شيفرة أو مظلّلات أو أصول من اللعبة، ولا يساعد في الحصول عليها.

</div>
