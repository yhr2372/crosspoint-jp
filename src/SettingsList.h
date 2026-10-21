#pragma once

#include <HalGPIO.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

// Build the font family setting dynamically. When registry is non-null, SD card fonts
// are appended after the built-in fonts. Otherwise only built-in fonts are listed.
inline SettingInfo buildFontFamilySetting(const SdCardFontRegistry* registry) {
  // Built-in font labels (StrId)
  std::vector<StrId> enumValues = {StrId::STR_NOTO_SERIF, StrId::STR_NOTO_SANS, StrId::STR_OPEN_DYSLEXIC};
  // Runtime string labels for SD card fonts
  std::vector<std::string> enumStringValues;

  // Reserve: first CrossPointSettings::BUILTIN_FONT_COUNT entries use StrId, rest use strings
  if (registry) {
    const auto& families = registry->getFamilies();
    enumStringValues.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(enumStringValues),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  // Capture the SD font count for the lambdas
  const int sdFontCount = static_cast<int>(enumStringValues.size());

  // Total option count = built-in + SD card families
  // For the combined enumStringValues: we need all entries as strings (built-in names + SD names)
  // The render code checks enumStringValues first, then enumValues. So we build enumStringValues
  // with all options when SD fonts are present.
  std::vector<std::string> allStringValues;
  if (sdFontCount > 0) {
    allStringValues.push_back(I18N.get(StrId::STR_NOTO_SERIF));
    allStringValues.push_back(I18N.get(StrId::STR_NOTO_SANS));
    allStringValues.push_back(I18N.get(StrId::STR_OPEN_DYSLEXIC));
    allStringValues.insert(allStringValues.end(), enumStringValues.begin(), enumStringValues.end());
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_FAMILY;
  s.type = SettingType::ENUM;
  s.enumValues = std::move(enumValues);
  s.enumStringValues = std::move(allStringValues);
  s.key = "fontFamily";
  s.category = StrId::STR_CAT_READER;

  // Capture registry families by copy for the lambdas
  std::vector<std::string> sdFamilyNames;
  if (registry) {
    const auto& families = registry->getFamilies();
    sdFamilyNames.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilyNames),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  s.valueGetter = [sdFamilyNames]() -> uint8_t {
    // If an SD card font is selected, find its index
    if (SETTINGS.horizontal.sdFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(sdFamilyNames.size()); i++) {
        if (sdFamilyNames[i] == SETTINGS.horizontal.sdFontFamilyName) {
          return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
        }
      }
      // SD font name not found in registry — fall through to built-in
    }
    return SETTINGS.horizontal.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.horizontal.fontFamily : 0;
  };

  s.valueSetter = [sdFamilyNames](uint8_t v) {
    if (v < CrossPointSettings::BUILTIN_FONT_COUNT) {
      SETTINGS.horizontal.fontFamily = v;
      SETTINGS.horizontal.sdFontFamilyName[0] = '\0';
    } else {
      int sdIdx = v - CrossPointSettings::BUILTIN_FONT_COUNT;
      if (sdIdx < static_cast<int>(sdFamilyNames.size())) {
        strncpy(SETTINGS.horizontal.sdFontFamilyName, sdFamilyNames[sdIdx].c_str(),
                sizeof(SETTINGS.horizontal.sdFontFamilyName) - 1);
        SETTINGS.horizontal.sdFontFamilyName[sizeof(SETTINGS.horizontal.sdFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
// Pass registry to include SD card fonts in the font family setting.
inline std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr) {
  // 子設定の表示条件（親の値に依存する項目は、親がその機能を使うときだけ端末に表示する）
  using S = CrossPointSettings;
  constexpr auto sleepUsesBitmap = [] {
    // カスタム画像・カバーは BMP を描くので、収め方やフィルターが意味を持つ
    return SETTINGS.sleepScreen == S::CUSTOM || SETTINGS.sleepScreen == S::COVER ||
           SETTINGS.sleepScreen == S::COVER_CUSTOM;
  };
  constexpr auto sleepUsesCover = [] {
    return SETTINGS.sleepScreen == S::COVER || SETTINGS.sleepScreen == S::COVER_CUSTOM;
  };
  constexpr auto sleepIsDynamicWallpaper = [] { return SETTINGS.sleepScreen == S::DYNAMIC_WALLPAPER; };
  // DS3231 を持つ X3 だけが RTC を使える。カレンダーの重ね描きは RTC で日付が保てるときだけ意味がある
  constexpr auto hasRtc = [] { return gpio.deviceIsX3(); };
  constexpr auto calendarAvailable = [] { return gpio.deviceIsX3() && SETTINGS.rtcEnabled != 0; };
  constexpr auto calendarOn = [] {
    return gpio.deviceIsX3() && SETTINGS.rtcEnabled != 0 && SETTINGS.sleepCalendar != 0;
  };
  // 動的壁紙は日付が必要。スリープは電源断なので、DS3231 を使う（X3 で RTC 有効）以外では
  // 起きた時点で時刻が失われ、既定のスリープ画面に退避する。その事情を設定画面で示す
  constexpr auto dynamicWallpaperNeedsRtc = [] {
    return gpio.deviceIsX3() && SETTINGS.sleepScreen == S::DYNAMIC_WALLPAPER && SETTINGS.rtcEnabled == 0;
  };
  constexpr auto dynamicWallpaperNoRtcDevice = [] {
    return gpio.deviceIsX4() && SETTINGS.sleepScreen == S::DYNAMIC_WALLPAPER;
  };

  std::vector<SettingInfo> v = {
      // --- Display ---
      // スリープ画面（親）
      SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                        {StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER, StrId::STR_NONE_OPT,
                         StrId::STR_COVER_CUSTOM, StrId::STR_DYNAMIC_WALLPAPER},
                        "sleepScreen", StrId::STR_CAT_DISPLAY),
      //   └ カバー: 収め方
      SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                        {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY)
          .dependsOn(sleepUsesCover),
      //   └ カスタム・カバー: 画像フィルター
      SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                        {StrId::STR_FILTER_GRAYSCALE, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                        "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY)
          .dependsOn(sleepUsesBitmap),
      //   └ 動的壁紙: 種類
      SettingInfo::Enum(StrId::STR_DYNAMIC_WALLPAPER_STYLE, &CrossPointSettings::dynamicWallpaperStyle,
                        {StrId::STR_DW_WATER_LEVEL, StrId::STR_DW_DOT_GRID, StrId::STR_DW_SQUARE_GRID},
                        "dynamicWallpaperStyle", StrId::STR_CAT_DISPLAY)
          .dependsOn(sleepIsDynamicWallpaper),
      //   └ 動的壁紙の前提の案内。X3 で RTC 無効なら押すと本体タブの「RTC 有効」へ移動
      SettingInfo::Info(StrId::STR_DW_NEEDS_RTC, SettingAction::JumpToRtcSetting, StrId::STR_CAT_DISPLAY)
          .dependsOn(dynamicWallpaperNeedsRtc),
      SettingInfo::Info(StrId::STR_DW_NO_RTC_DEVICE, SettingAction::None, StrId::STR_CAT_DISPLAY)
          .dependsOn(dynamicWallpaperNoRtcDevice),
      //   └ カレンダーを重ねる（X3 かつ RTC 有効のとき）
      SettingInfo::Toggle(StrId::STR_SLEEP_CALENDAR, &CrossPointSettings::sleepCalendar, "sleepCalendar",
                          StrId::STR_CAT_DISPLAY)
          .dependsOn(calendarAvailable),
      //       └ カレンダー配置
      SettingInfo::Enum(StrId::STR_SLEEP_CALENDAR_POSITION, &CrossPointSettings::sleepCalendarPosition,
                        {StrId::STR_CALENDAR_POS_TOP, StrId::STR_CALENDAR_POS_CENTER, StrId::STR_CALENDAR_POS_BOTTOM},
                        "sleepCalendarPosition", StrId::STR_CAT_DISPLAY)
          .dependsOn(calendarOn, 2),
      SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                        {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                        StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(
          StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
          {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
          "refreshFrequency", StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
                        {StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED, StrId::STR_THEME_VEGA}, "theme",
                        StrId::STR_CAT_DISPLAY),
      SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                          StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_COLOR_MODE, &CrossPointSettings::colorMode, {StrId::STR_LIGHT, StrId::STR_DARK},
                        "colorMode", StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_UI_ORIENTATION, &CrossPointSettings::uiOrientation,
                        {StrId::STR_PORTRAIT, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CW, StrId::STR_LANDSCAPE_CCW},
                        "uiOrientation", StrId::STR_CAT_DISPLAY),

      // --- Reader ---
      SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                          StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                        {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW},
                        "orientation", StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                        {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                        "imageRendering", StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_WRITING_MODE, &CrossPointSettings::writingMode,
                        {StrId::STR_WM_AUTO, StrId::STR_WM_HORIZONTAL, StrId::STR_WM_VERTICAL}, "writingMode",
                        StrId::STR_CAT_READER),
      SettingInfo::Toggle(StrId::STR_INVERT_IMAGES, &CrossPointSettings::invertImages, "invertImages",
                          StrId::STR_CAT_READER),
      // 省メモリ（SdCardFont::LOAD_*）。変えると SD フォントを読み直し、章のレイアウトも作り直す
      SettingInfo::Toggle(StrId::STR_KERN_LATIN_ONLY, &CrossPointSettings::kernLatinOnly, "kernLatinOnly",
                          StrId::STR_CAT_READER),
      SettingInfo::Toggle(StrId::STR_SYNTHETIC_BOLD, &CrossPointSettings::syntheticBold, "syntheticBold",
                          StrId::STR_CAT_READER),

      // --- Controls ---
      SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                        {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV}, "sideButtonLayout", StrId::STR_CAT_CONTROLS),
      SettingInfo::Toggle(StrId::STR_LONG_PRESS_SKIP, &CrossPointSettings::longPressChapterSkip, "longPressChapterSkip",
                          StrId::STR_CAT_CONTROLS),
      SettingInfo::Enum(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
                        {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH},
                        "shortPwrBtn", StrId::STR_CAT_CONTROLS),

      // --- System ---
      SettingInfo::Enum(StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeout,
                        {StrId::STR_MIN_1, StrId::STR_MIN_5, StrId::STR_MIN_10, StrId::STR_MIN_15, StrId::STR_MIN_30},
                        "sleepTimeout", StrId::STR_CAT_SYSTEM),
      // RTC（DS3231）を使うか。X3 のみ。スリープ中も時刻を保つが電池を消費する
      SettingInfo::Toggle(StrId::STR_RTC_ENABLED, &CrossPointSettings::rtcEnabled, "rtcEnabled", StrId::STR_CAT_SYSTEM)
          .dependsOn(hasRtc, 0),
      SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                          StrId::STR_CAT_SYSTEM),
      SettingInfo::Toggle(StrId::STR_DEBUG_DISPLAY, &CrossPointSettings::debugDisplay, "debugDisplay",
                          StrId::STR_CAT_SYSTEM),
      // ラベル数は CrossPointSettings::OTA_CHANNEL_COUNT と一致させること。
      // OtaUpdater.cpp の static_assert が enum 側のずれは検出するが、
      // このラベル数（vector のサイズ）だけはコンパイル時に縛れない。
      SettingInfo::Enum(StrId::STR_OTA_CHANNEL, &CrossPointSettings::otaChannel,
                        {StrId::STR_OTA_CHANNEL_STABLE, StrId::STR_OTA_CHANNEL_RC, StrId::STR_OTA_CHANNEL_DEV},
                        "otaChannel", StrId::STR_CAT_SYSTEM),

      // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
      SettingInfo::DynamicString(
          StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
            KOREADER_STORE.saveToFile();
          },
          "koUsername", StrId::STR_KOREADER_SYNC),
      SettingInfo::DynamicString(
          StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
            KOREADER_STORE.saveToFile();
          },
          "koPassword", StrId::STR_KOREADER_SYNC),
      SettingInfo::DynamicString(
          StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
          [](const std::string& v) {
            KOREADER_STORE.setServerUrl(v);
            KOREADER_STORE.saveToFile();
          },
          "koServerUrl", StrId::STR_KOREADER_SYNC),
      SettingInfo::DynamicEnum(
          StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
          [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
          [](uint8_t v) {
            KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
            KOREADER_STORE.saveToFile();
          },
          "koMatchMethod", StrId::STR_KOREADER_SYNC),

      // --- OPDS Browser (web-only, uses CrossPointSettings char arrays) ---
      SettingInfo::String(StrId::STR_OPDS_SERVER_URL, SETTINGS.opdsServerUrl, sizeof(SETTINGS.opdsServerUrl),
                          "opdsServerUrl", StrId::STR_OPDS_BROWSER),
      SettingInfo::String(StrId::STR_USERNAME, SETTINGS.opdsUsername, sizeof(SETTINGS.opdsUsername), "opdsUsername",
                          StrId::STR_OPDS_BROWSER),
      SettingInfo::String(StrId::STR_PASSWORD, SETTINGS.opdsPassword, sizeof(SETTINGS.opdsPassword), "opdsPassword",
                          StrId::STR_OPDS_BROWSER)
          .withObfuscated(),
      // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
      SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                          "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR),
      SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage,
                          "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                        {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                        StrId::STR_CUSTOMISE_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                        {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                        "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                        {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                        StrId::STR_CUSTOMISE_STATUS_BAR),
      SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
  };
  // X3 only — show tilt page turn setting when the QMI8658 IMU is present.
  if (halTiltSensor.isAvailable()) {
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (it->nameId == StrId::STR_SHORT_PWR_BTN) {
        v.insert(it + 1, SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                                           {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED},
                                           "tiltPageTurn", StrId::STR_CAT_CONTROLS));
        break;
      }
    }
  }
  return v;
}
