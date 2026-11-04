# Security Policy

[English](#english) | [Українська](#українська)

## English

### Supported versions

This project is under active development. Security fixes are applied only to
the latest revision of the default branch. Older commits, forks, modified
firmware, and third-party binaries are not supported.

| Version | Security support |
|---|---|
| Latest default-branch revision | Supported |
| Older revisions and forks | Not supported |

### Reporting a vulnerability

Please report suspected vulnerabilities privately by email to
**lex.vyshnevskyy@gmail.com** with the subject:

```text
[crsf-bridge security] Short description
```

Do not open a public GitHub issue for an unpatched vulnerability. Include, when
available:

- the affected commit, firmware environment, and hardware configuration;
- a clear description of the vulnerability and its potential impact;
- reproduction steps or a minimal proof of concept;
- relevant logs, packet captures, or configuration details;
- any proposed mitigation or patch;
- whether the issue has been disclosed elsewhere.

Remove passwords, private keys, precise locations, personal data, and other
unrelated secrets before sending a report.

### Response and disclosure

The maintainer will try to acknowledge a complete report within **7 days** and
provide an initial assessment within **14 days**. These are targets, not a
service-level agreement.

Please allow reasonable time to investigate and publish a fix before public
disclosure. The maintainer may request additional information, coordinate a
release date, and credit the reporter unless anonymity is requested. Reports
that cannot be reproduced may be closed after the available evidence has been
reviewed.

### Scope

Security-relevant reports may include:

- memory-safety or input-validation defects in the firmware or host tools;
- unintended transmission, corruption, or disclosure of CRSF data;
- unsafe bootloader/programmer behavior that can overwrite an unintended
  target;
- repository-controlled build or dependency issues;
- documentation errors that create a repeatable electrical or operational
  safety hazard.

The following are normally outside project security support:

- vulnerabilities in CH9121 firmware, NetModuleConfig, operating systems,
  network equipment, radios, flight controllers, or other third-party
  products;
- exposed UDP/TCP services caused solely by deployment without network access
  controls;
- physical access attacks, component counterfeits, soldering defects, or
  modifications not present in this repository;
- requests for general setup help or feature changes.

Third-party issues should also be reported to their respective vendor. A
report is still welcome when this project's defaults or documentation make a
third-party issue materially more dangerous.

### Deployment and hardware safety

This bridge does not provide authentication, encryption, replay protection, or
authorization. Treat the CH9121 network as a trusted control network. Do not
expose its TCP or UDP ports directly to the public Internet. Use network
segmentation, firewall rules, or a secure authenticated tunnel when traffic
must cross an untrusted network.

Test with propellers removed and actuators made safe. Verify power polarity,
3.3 V logic levels, common ground, wiring, UART rates, failsafe behavior, and
control direction before operational use. Never rely on this experimental
project as the only safety mechanism for people, property, or aircraft.

The bundled `NetModuleConfig.exe` is a third-party binary and is not built from
this repository's source. Verify its published checksum and assess it under
your own security policy before execution.

## Українська

### Підтримувані версії

Проєкт перебуває в активній розробці. Виправлення безпеки застосовуються лише
до останньої редакції основної гілки. Старі коміти, форки, змінені прошивки та
сторонні виконувані файли не підтримуються.

| Версія | Підтримка безпеки |
|---|---|
| Остання редакція основної гілки | Підтримується |
| Старі редакції та форки | Не підтримуються |

### Повідомлення про вразливість

Надсилайте інформацію про можливі вразливості приватно на адресу
**lex.vyshnevskyy@gmail.com** із темою:

```text
[crsf-bridge security] Короткий опис
```

Не створюйте публічний GitHub issue для ще не виправленої вразливості. За
можливості додайте:

- уражений коміт, середовище прошивки й конфігурацію обладнання;
- чіткий опис вразливості та можливих наслідків;
- кроки відтворення або мінімальний proof of concept;
- відповідні журнали, дампи пакетів або параметри конфігурації;
- запропонований спосіб усунення чи патч;
- інформацію про те, чи була проблема оприлюднена деінде.

Перед надсиланням видаліть паролі, приватні ключі, точні координати, персональні
дані та інші секрети, не потрібні для аналізу.

### Реагування та оприлюднення

Автор намагатиметься підтвердити отримання повного звіту протягом **7 днів** і
надати початкову оцінку протягом **14 днів**. Це орієнтовні строки, а не угода
про рівень обслуговування.

До публічного розкриття дайте достатньо часу для перевірки та випуску
виправлення. Автор може запросити додаткові дані, узгодити дату публікації та
зазначити автора звіту, якщо він не попросив про анонімність.

### Область дії

До питань безпеки можуть належати:

- помилки безпеки пам'яті або перевірки вхідних даних у прошивці та утилітах;
- ненавмисне передавання, пошкодження або розкриття даних CRSF;
- небезпечна поведінка bootloader/programmer, що може перезаписати не той
  пристрій;
- проблеми контрольованої репозиторієм збірки або залежностей;
- помилки документації, які створюють відтворювану електричну чи операційну
  небезпеку.

Зазвичай поза межами підтримки:

- вразливості CH9121, NetModuleConfig, операційних систем, мережевого
  обладнання, пультів, польотних контролерів та інших сторонніх продуктів;
- відкриті UDP/TCP-сервіси, спричинені лише розгортанням без мережевих обмежень;
- атаки з фізичним доступом, підроблені компоненти, дефекти паяння або зміни,
  яких немає в цьому репозиторії;
- загальні питання налаштування та запити нових функцій.

### Безпечне розгортання й робота з обладнанням

Міст не забезпечує автентифікацію, шифрування, захист від повторного
відтворення пакетів або авторизацію. Вважайте мережу CH9121 довіреною мережею
керування. Не відкривайте її TCP/UDP-порти безпосередньо в Інтернет. Для
передавання через недовірену мережу використовуйте сегментацію, firewall або
захищений автентифікований тунель.

Проводьте випробування зі знятими пропелерами та безпечно відключеними
виконавчими механізмами. До експлуатації перевірте полярність живлення, рівні
логіки 3,3 В, спільну землю, проводку, швидкості UART, failsafe і напрямок
керування. Не використовуйте цей експериментальний проєкт як єдиний механізм
захисту людей, майна або літального апарата.

Доданий `NetModuleConfig.exe` є стороннім виконуваним файлом і не збирається з
вихідного коду цього репозиторію. Перед запуском перевірте опубліковану
контрольну суму та оцініть файл відповідно до власної політики безпеки.
