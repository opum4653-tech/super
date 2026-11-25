# Firewall Domain Logger Guide (`firewall.txt`)

এই গাইডে দেখানো হয়েছে কীভাবে গেমের আসল আউটবাউন্ড ডোমেইনগুলো `/sdcard/Download/firewall.txt`-এ সেভ করা হয়েছে এবং টেস্ট শেষ হলে কীভাবে সম্পূর্ণ রিমুভ (বা আবার যোগ) করবেন।

---

## ১. এটি কীভাবে কাজ করে?
- ফাইল: `src/other/domain_logger.hpp`
- এটি টার্গেট গেম (`com.dts.freefiremax`, `proc::pid`) চালু থাকলে তার Linux UID বের করে।
- আউটবাউন্ড `TCP:443` (HTTPS TLS `ClientHello` SNI) প্যাকেট থেকে আসল ডোমেইন নাম পড়ে এবং `/proc/net/tcp` ও `/proc/net/tcp6` দিয়ে নিশ্চিত করে যে সকেটটি গেমের UID-এরই।
- ডুপ্লিকেট বাদ দিয়ে প্রতিটি নতুন আসল ডোমেইন `/sdcard/Download/firewall.txt` (ফলব্যাক `/storage/emulated/0/Download/firewall.txt`)-এ এক লাইনে সেভ করে।

---

## ২. টেস্ট শেষে রিমুভ করার নিয়ম (Remove Guide)

টেস্ট হয়ে গেলে ডোমেইন লগার সম্পূর্ণ সরাতে নিচের **২টি ধাপ** করুন:

1. **ফাইল ডিলিট করুন:**
   - `src/other/domain_logger.hpp` ডিলিট করে দিন।

2. **`src/main.cpp` থেকে ৩টি লাইন মুছে দিন:**
   - `#include "other/domain_logger.hpp"`
   - `domain_logger::start();`
   - `domain_logger::stop();`

*(যেহেতু `domain_logger.hpp` header-only, তাই `jni/Android.mk` বা `CMakeLists.txt`-এ কোনো পরিবর্তন করতে হয় না।)*

---

## ৩. ভবিষ্যতে আবার যোগ করার নিয়ম (Re-Add Guide)

1. `src/other/domain_logger.hpp` ফাইলটি রাখুন।
2. `src/main.cpp`-এ:
   - হেডার ইনক্লুড করুন: `#include "other/domain_logger.hpp"`
   - `start_overlay()`-এর শুরুতে `kpm.init(true);`-এর পর লিখুন: `domain_logger::start();`
   - `start_overlay()`-এর এক্সিট ক্লিনআপে `shutdown();`-এর আগে লিখুন: `domain_logger::stop();`
3. নতুন করে বিল্ড করে রান করলেই `/sdcard/Download/firewall.txt` তৈরি/আপডেট হবে।
