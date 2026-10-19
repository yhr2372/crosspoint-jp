#pragma once

#include <cstdint>

namespace VerticalTextUtils {

// Character behavior in vertical text layout
enum class VerticalBehavior : uint8_t {
  Upright,      // CJK ideographs, kana - draw normally, advance downward
  Sideways,     // Latin letters, 3+ digit numbers - rotate 90 CW
  TateChuYoko,  // 1-2 digit numbers - horizontal-in-vertical
};

// 縦書きでも立てたまま描く記号（UAX #50 の vo=U のうち日本語の本文に出るもの）。
// ☗☖▲△（棋譜）、①、℃、Ⅲ、○●□■、★、♪、※、×÷± など。
// 矢印（U+2190..21FF）・罫線（U+2500..257F、ダッシュ代わりの──）・≒などの数学記号は
// 縦書きでは向きを回すのが正しいので含めない。
inline bool isUprightSymbol(uint32_t cp) {
  if (cp == 0x00A7 || cp == 0x00A9 || cp == 0x00AE || cp == 0x00B1 || cp == 0x00B6) return true;  // § © ® ± ¶
  if (cp == 0x00D7 || cp == 0x00F7) return true;                                                  // × ÷
  if (cp == 0x203B) return true;                                                                  // ※
  if (cp >= 0x2100 && cp <= 0x218F) return true;                  // Letterlike Symbols, Number Forms
  if (cp == 0x221E || cp == 0x2234 || cp == 0x2235) return true;  // ∞ ∴ ∵
  if (cp >= 0x2460 && cp <= 0x24FF) return true;                  // Enclosed Alphanumerics
  if (cp >= 0x25A0 && cp <= 0x27BF) return true;                  // Geometric Shapes, Misc Symbols, Dingbats
  if (cp >= 0x2B50 && cp <= 0x2B59) return true;                  // ⭐ etc.
  if (cp >= 0x3190 && cp <= 0x319F) return true;                  // Kanbun
  if (cp >= 0x31F0 && cp <= 0x31FF) return true;                  // Katakana Phonetic Extensions
  return false;
}

// Determine if a codepoint should be drawn upright in vertical text.
// CJK ideographs, kana, CJK symbols, fullwidth forms, etc.
inline bool isUprightInVertical(uint32_t cp) {
  if (isUprightSymbol(cp)) return true;
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;  // CJK Unified Ideographs
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;  // CJK Extension A
  if (cp >= 0x3040 && cp <= 0x309F) return true;  // Hiragana
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;  // Katakana
  if (cp >= 0x3000 && cp <= 0x303F) return true;  // CJK Symbols and Punctuation
  if (cp >= 0xFF00 && cp <= 0xFFEF) return true;  // Fullwidth Forms
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;  // CJK Compatibility Ideographs
  if (cp >= 0x3200 && cp <= 0x32FF) return true;  // Enclosed CJK Letters
  if (cp >= 0x3300 && cp <= 0x33FF) return true;  // CJK Compatibility
  if (cp >= 0x3100 && cp <= 0x312F) return true;  // Bopomofo
  if (cp >= 0xAC00 && cp <= 0xD7AF) return true;  // Hangul
  return false;
}

// Vertical repositioning for small kana (小書き仮名), as a percentage of the
// full-width advance (em). In horizontal layout a small kana glyph is drawn
// low in the em box and roughly centred horizontally; vertical layout places
// it toward the upper right (JLREQ 3.1.4 / JIS X 4051).
//
// The magnitudes below are measured, not guessed. For each of the 24 small
// kana the OpenType 'vert' substitute was compared against its base glyph in
// seven typefaces, and the displacement taken as the shift of the ink
// bounding box *centre*:
//
//   NotoSansCJKjp Regular  +0.112 em right  +0.106 em up
//   NotoSansCJKjp Bold     +0.104           +0.106
//   Hiragino Kaku Gothic W3 +0.112          +0.108
//   Hiragino Kaku Gothic W6 +0.103          +0.098
//   Hiragino Mincho ProN   +0.114           +0.099
//   Hiragino Maru Gothic W4 +0.102          +0.094
//   Hiragino Sans GB       +0.111           +0.108
//   ------------------------------------------------
//   mean                   +0.1083          +0.1028
//
// NotoSansCJKjp matters most here: it is the source of the bundled .cpfont
// files (lib/EpdFont/scripts/downloaded_fonts/).
//
// Each row is a mean over the 24 characters (166 glyph pairs in total). The
// means are tight across typefaces, but individual characters are not: the
// per-character standard deviation is 0.013 em horizontally and 0.020 em
// vertically (full spread 0.073..0.158 and 0.060..0.166). A single constant
// is a deliberate approximation of that spread - a per-character table would
// cost flash and complexity for a correction whose residual error is well
// under one pixel at the em sizes this firmware renders (25-38 px).
//
// Centre rather than corner is the right reference because we translate the
// unmodified horizontal glyph. Hiragino's vert forms are pure translations so
// the two agree, but Noto redraws them slightly smaller, and aligning corners
// would then overshoot to the upper right by the size difference.
//
// Note the offsets are deliberately derived from the advance alone and not
// from the glyph ink box: EpdGlyph width/height/left/top are post-rasterisation
// bitmap extents, so at a ~30 px em a one-pixel cropping difference is already
// 0.03 em of error. The advance carries no such error.
static constexpr int SMALL_KANA_DX_PERCENT = 11;  // rightward, % of em
static constexpr int SMALL_KANA_DY_PERCENT = 10;  // upward, % of em

// Is this codepoint a small kana needing the offset above in vertical text?
inline constexpr bool isSmallKana(uint32_t cp) {
  if (cp >= 0x3041 && cp <= 0x3096) {  // Hiragana
    switch (cp) {
      case 0x3041:  // ぁ
      case 0x3043:  // ぃ
      case 0x3045:  // ぅ
      case 0x3047:  // ぇ
      case 0x3049:  // ぉ
      case 0x3063:  // っ
      case 0x3083:  // ゃ
      case 0x3085:  // ゅ
      case 0x3087:  // ょ
      case 0x308E:  // ゎ
      case 0x3095:  // ゕ
      case 0x3096:  // ゖ
        return true;
      default:
        return false;
    }
  }
  if (cp >= 0x30A1 && cp <= 0x30F6) {  // Katakana
    switch (cp) {
      case 0x30A1:  // ァ
      case 0x30A3:  // ィ
      case 0x30A5:  // ゥ
      case 0x30A7:  // ェ
      case 0x30A9:  // ォ
      case 0x30C3:  // ッ
      case 0x30E3:  // ャ
      case 0x30E5:  // ュ
      case 0x30E7:  // ョ
      case 0x30EE:  // ヮ
      case 0x30F5:  // ヵ
      case 0x30F6:  // ヶ
        return true;
      default:
        return false;
    }
  }
  // Katakana Phonetic Extensions - small katakana for Ainu (ㇰㇱㇲ...).
  // All 16 are small forms. Their measured displacement (+0.102 em right,
  // +0.091 em up) sits within the spread of the main set, so they share the
  // same constants.
  if (cp >= 0x31F0 && cp <= 0x31FF) return true;
  // Halfwidth small katakana (U+FF67..FF6F) are excluded on purpose, even
  // though isUprightInVertical() above sends them down the same upright path.
  // Two reasons: none of the seven typefaces measured carries a 'vert'
  // substitute for them, so there is nothing to derive an offset from; and
  // the model does not apply anyway, since a halfwidth kana fills its narrow
  // cell rather than sitting small and low inside a full-width em box.
  // (Four of the Hiragino faces do define halfwidth forms under 'vrt2', but
  // fontconvert_sdcard.py reads only 'vert', so they never reach this code.)
  return false;
}

// Should this codepoint use the OpenType 'vert' substitute glyph?
// Returns true only for punctuation, brackets, and long marks that need
// a different glyph shape in vertical text. Kana and ideographs are excluded
// because their vert variants differ only in metrics (designed for use with
// a full shaping engine), and bitmap-only substitution looks wrong.
inline bool shouldUseVertGlyph(uint32_t cp) {
  // CJK punctuation and brackets (3000-303F), excluding ideographs like 〆(3006)
  if (cp == 0x3001 || cp == 0x3002) return true;  // 、。
  if (cp >= 0x3008 && cp <= 0x3011) return true;  // 〈〉《》「」『』【】
  if (cp >= 0x3014 && cp <= 0x301B) return true;  // 〔〕〖〗〘〙〚〛
  if (cp >= 0x301D && cp <= 0x301F) return true;  // 〝〞〟
  // Fullwidth punctuation and brackets
  if (cp == 0xFF01 || cp == 0xFF1F) return true;  // ！？
  if (cp == 0xFF08 || cp == 0xFF09) return true;  // （）
  if (cp == 0xFF0C || cp == 0xFF0E) return true;  // ，．
  if (cp == 0xFF1A || cp == 0xFF1B) return true;  // ：；
  if (cp == 0xFF3B || cp == 0xFF3D) return true;  // ［］
  if (cp == 0xFF5B || cp == 0xFF5D) return true;  // ｛｝
  if (cp == 0xFF5E) return true;                  // ～
  // Long marks and dashes
  if (cp == 0x30FC) return true;                  // ー
  if (cp == 0x2014 || cp == 0x2015) return true;  // —―
  if (cp == 0x2025 || cp == 0x2026) return true;  // ‥…
  if (cp == 0x22EF) return true;                  // ⋯
  return false;
}

// 禁則処理（行頭禁則・行末禁則・分離禁止）は横書きと縦書きで共通なので、
// lib/Epub/Epub/Kinsoku.h に移した。小書き仮名の判定 isSmallKana() だけは
// 縦書きのグリフ配置補正にも使うのでここに残してある。

}  // namespace VerticalTextUtils
