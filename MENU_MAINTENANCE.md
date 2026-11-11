# MENU MAINTENANCE RULEBOOK

A practical engineering reference and maintenance rulebook for future developers working on the Dear ImGui Android overlay project.

---

## 1. Golden Rules
- **Style Stack Balance**: প্রতিটা `ImGui::PushStyleColor` এর জন্য অবিকল পেয়ার্ড `ImGui::PopStyleColor` এবং প্রতিটা `ImGui::PushStyleVar` এর জন্য পেয়ার্ড `ImGui::PopStyleVar` থাকতে হবে। স্ট্যাকের একটির অমিলও সমগ্র ImGui ফ্রেম ক্র্যাশ করাবে।
- **Window/Child Lifetime**: প্রতিটা `ImGui::Begin` এর বিপরীতে `ImGui::End` এবং প্রতিটা `ImGui::BeginChild` এর বিপরীতে `ImGui::EndChild` থাকতে হবে।
- **Exit & Shutdown Safety**: `exit()` বা `Touch_Close()` কখনো ImGui রেন্ডার ফ্রেমের ভেতরে সরাসরি কল করা যাবে না। ফ্রেমের বাইরে সেফ এক্সিটের জন্য শুধুমাত্র `g_should_exit = true` ফ্ল্যাগ সেট করুন।
- **License Gating**: `func::esp::draw()` এবং `func::aim::update()` কখনো লাইসেন্স গেট `lic::is_valid()` এর বাইরে কল করা যাবে না।

---

## 2. Layout Geometry (Hardcoded Specifications)
- **Main Window Dimension**: `ImMin(820.0f, realW * 0.94f)` × `ImMin(460.0f, realH * 0.94f)`.
- **Header Height**: `44.0f` px.
- **Sidebar Width**: `60.0f` px.
- **Content Area Offset**: `(sidebarW + 14.0f, headerH + 10.0f)`.
- **Dual Column Gap**: `14.0f` px.
- **Card Padding & Spacing**: WindowPadding `(14.0f, 10.0f)`, ItemSpacing `(8.0f, 10.0f)`.
- **Touch Target Size**: সব বোতাম ও ইন্টারেক্টিভ এলিমেন্ট ন্যূনতম `36 × 36` px হতে হবে।
- **Screen Res Testing**: `720 × 1280` (HD) এবং `1440 × 3200` (QHD+) দুই রেজোলিউশনেই লেআউট টেস্ট করতে হবে।

---

## 3. Color Rules (Theme Orchid Foundation)
- সমস্ত UI এলিমেন্টের রঙ সেন্ট্রালাইজড `theme_orchid` নেমস্পেস থেকে আসতে হবে। কোনো ইনলাইন ম্যাজিক হেক্স কোড ব্যবহার করবেন না।
- **কালার প্যালেট রেফারেন্স**:
  - `theme_orchid::ColPrimary` = `#E10098` (ইম্পেরিয়াল অর্কিড - অ্যাকসেন্ট ও বার)
  - `theme_orchid::ColPrimaryLight` = `#FF5ACD` (হোভার ও হাইলাইট)
  - `theme_orchid::ColHeaderBg` = `#15101F` (ডার্ক হেডারের ব্যাকগ্রাউন্ড)
  - `theme_orchid::ColTextMuted` = `#9B8CB4` (সেকেন্ডারি টেক্সট ও সাবটাইটেল)
  - `theme_orchid::ColWhite` = `#FCF8FF` (ক্রিস্প সাদা টেক্সট ও আইকন)
- নতুন কোনো রঙের প্রয়োজন হলে তা সরাসরি `src/ui/theme.hpp`-এর `theme_orchid` নেমস্পেসে যোগ করুন।

---

## 4. Touch Gesture & Scroll Engine
- প্রতিটা স্ক্রোলেবল চাইল্ড উইন্ডোর শেষে `handleTouchScroll()` কল করতে হবে (`EndChild()`-এর ঠিক আগে)।
- **Gesture Threshold**: অনুভূমিক ও উল্লম্ব ডিসক্রিমিনেশনে `5.0f` px মুভমেন্ট থ্রেশহোল্ড এবং `1.1f` ডমিন্যান্স রেশিও অপরিবর্তিত রাখুন।
- **ClearActiveID Integrity**: উল্লম্ব স্ক্রোল ডিটেক্ট হলে (`GESTURE_VERTICAL`) সাথে সাথে `ImGui::ClearActiveID()` কল হয়, যা স্লাইডারের আইডি দখল বাতিল করে স্ক্রিন স্ক্রোল চালু করে। এটি কখনো সরাবেন না।

---

## 5. License Integration Architecture (Strict CLI Terminal Only)
- লাইসেন্স ভেরিফিকেশন সম্পূর্ণ টার্মিনাল-লেভেল (CLI)। ব্যবহারকারী টার্মিনালে `./client.sh` রান করলে সেখানেই লাইসেন্স কি চাওয়া হয়।
- লাইসেন্স সঠিক হলে টার্মিনালে ভিআইপি ড্যাশবোর্ড প্রিন্ট হয় এবং তবেই মেনু ও ওভারলে রেন্ডার শুরু হয়।
- ভুল লাইসেন্স বা ডেট এক্সপায়ার হলে টার্মিনাল সরাসরি এক্সিট করে—কোনো অবস্থাতেই কোনো GUI ওভারলে চালু হয় না।
- ImGui মেনুর ভেতরে কোনো লাইসেন্স মোডাল, পপআপ, বা কি ইনপুট বক্স রাখা যাবে না। মেনুর ভেতরে জিরো লাইসেন্স UI।

---

## 6. Aim Assist Notes
- **Trigger Modes**: `0 = When Firing`, `1 = Always In FOV`।
- **IsScoping Safety**: গেমের মেমোরিতে সরাসরি কোনো `IsScoping` ফিল্ড নেই, শুধু RVA মেথড রয়েছে যার ইনভোক এক্সটার্নাল মেমোরি মোডে ক্র্যাশ ও ডিটেকশন ঘটায়। তাই ট্রিগার মোড শুধুমাত্র ২টিতে সীমাবদ্ধ।
- **Sticky Bone Interpolation**: স্টিকি টার্গেট লকিংয়ের সময় `target_bone` সেটিং বজায় রাখা হয় (Head, Neck, Body)।
- **3D Ray Alignment**: মাজল অরিজিন ও হেড পজিশন ভ্যালিডেশন `sqrMagnitude() > 1e-6f` দ্বারা পরিচালিত হয় (`MagnitudeSquared()` নামে কোনো ফাংশন নেই)।

---

## 7. Adding a New Tab (ধাপভিত্তিক গাইড)
1. `src/ui/menu.cpp`-এর `EIconType` এনামে নতুন আইকন টাইপ যুক্ত করুন।
2. `DrawVectorIcon()` সুইচ কেসে ভেক্টর ড্রয়িং যোগ করুন।
3. সাইডবার লুপে `DrawSidebarIconButton()` কল করুন।
4. `render()` এর মূল বডিতে `else if (activeTab == N)` ব্লক যোগ করে দুটি `BeginChild` কলাম তৈরি করুন।
5. ল্যান্ডস্কেপ ও পোর্ট্রেট উভয় মোডে ওরিয়েন্টেশন ফ্লিপ টেস্ট করুন।
6. এই ফাইলে নতুন ট্যাবের কনফিগারেশন তালিকাভুক্ত করুন।

---

## 8. Adding a New Config Item
1. `src/ui/cfg.hpp`-এর উপযুক্ত নেমস্পেসে (`cfg::esp` বা `cfg::aim`) ভ্যারিয়েবল ডিক্লেয়ার করুন।
2. `src/ui/menu.cpp`-এ নির্দিষ্ট ট্যাবে উইজেট (`ImGui::Checkbox` বা `ImGui::SliderFloat`) যুক্ত করুন।
3. রেন্ডারারে (`esp.hpp` বা `aim.hpp`) কনফিগ গার্ড স্থাপন করুন।
4. লাইভ টগল টেস্ট করুন—অ্যাপ রিস্টার্ট ছাড়া তাৎক্ষণিক কাজ করতে হবে।

---

## 9. Testing Checklist (প্রতিটি কমিটের আগে যাচাই)
- [ ] মেনু ওপেন ও মিনিমাইজ পিল নির্বিঘ্নে কাজ করে।
- [ ] পিল ড্র্যাগ করলে মেনু খোলে না (Drag ≠ Tap ডিসক্রিমিনেশন কার্যকর)।
- [ ] মেনুর হেডার ড্র্যাগ করে স্ক্রিনে সরানো যায়।
- [ ] ফোন রোটেট করলে মেনু ও পিল অটো-রিসেন্টার এবং স্ক্রিন বাউন্ডারির ভেতরে ক্ল্যাম্প হয়।
- [ ] উভয় কলামের উচ্চতা ও প্যাডিং সমান থাকে।
- [ ] স্লাইডারে আনুভূমিক স্পর্শ করলে স্লাইডার নড়ে, উলম্ব স্ক্রোল হয় না।
- [ ] উলম্ব স্ক্রোল করার সময় স্লাইডার আটকে থাকে না বা মান পরিবর্তিত হয় না।
- [ ] পাওয়ার বোতাম চাপলে ক্লিন এক্সিট হয় (`g_should_exit`).
- [ ] লাইসেন্স এক্সপায়ার হলে মোডাল পপআপ মেনু আটকে দেয় এবং টেলিগ্রাম রিডাইরেক্ট করে।
- [ ] Logcat-এ কোনো ImGui Push/Pop assertion বা মেমোরি ফল্ট নেই।
- [ ] Android NDK r27+ Clang-এ `-Wall -Wextra` সহ সম্পূর্ণ পরিষ্কার কম্পাইল হয়।

---

## 10. Emergency Rollback Protocol
1. কোনো ক্র্যাশ ঘটলে তাৎক্ষণিক শেষ কমিট রিভার্ট করুন (`git revert HEAD`).
2. `PushStyleColor` ও `PushStyleVar` এর সংখ্যা চেক করুন (৯০% ImGui ক্র্যাশ স্ট্যাক ভারসাম্যহীনতার জন্য হয়)।
3. প্রতিটি `BeginChild` এর বিপরীতে সঠিক `EndChild` আছে কিনা যাচাই করুন।
4. মোডাল বন্ধ না হলে `ImGui::EndPopup()` কল হয়েছে কিনা দেখুন (`End()` নয়)।
5. অ্যাপ ফ্রিজ হলে রেন্ডার লুপের ভেতরে কোনো ব্লকিং সিস্টেম কল বা অবৈধ মেমোরি রিড আছে কিনা চেক করুন।
