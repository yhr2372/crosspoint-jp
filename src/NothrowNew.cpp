// `new (std::nothrow)` の置き換え。
//
// 標準ライブラリの nothrow 版は中で通常の new を呼び、失敗したら bad_alloc を投げて受け止める作りに
// なっている。このファームは例外を使わないので、ヒープが尽きたときは投げる時点で abort() に落ち、
// 「失敗したら nullptr を返す」はずの呼び出し側（SD フォントの字の読み込み・画像のデコードなど）の
// 後始末が一度も動かずに再起動していた（crash_report: SdCardFont::onGlyphMiss -> operator new[] -> abort）。
// malloc は失敗すると NULL を返すだけなので、それで直接確保する。
// delete / delete[] は libstdc++ の既定どおり free() で解放するので、対応は崩れない。
#include <cstdlib>
#include <new>

void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return std::malloc(size ? size : 1); }

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return std::malloc(size ? size : 1); }
