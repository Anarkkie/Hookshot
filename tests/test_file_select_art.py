"""Displayed File Select art must not prevent a build from replacing its PNGs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
RUN_ALL_SKIP = {"linux": "native Windows File Select uses GDI+ and Win32 file replacement"}
ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#define NOMINMAX
#include "file_select_store.h"
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <cwchar>
#include <iostream>
#include <memory>
using namespace file_select;
__PICTURE__
struct Menu {
    struct { fs::path root; } store;
__ART__
};
struct GraphicsSession {
    ULONG_PTR token = 0;
    GraphicsSession() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
            throw std::runtime_error("GDI+ startup failed");
    }
    ~GraphicsSession() { Gdiplus::GdiplusShutdown(token); }
};
CLSID png_encoder() {
    UINT count = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    std::vector<unsigned char> data(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(data.data());
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok)
        throw std::runtime_error("Image encoders unavailable");
    for (UINT i = 0; i < count; ++i)
        if (std::wcscmp(codecs[i].MimeType, L"image/png") == 0) return codecs[i].Clsid;
    throw std::runtime_error("PNG encoder unavailable");
}
void write_png(const fs::path& path, Gdiplus::ARGB color) {
    const auto encoder = png_encoder();
    Gdiplus::Bitmap image(2, 2, PixelFormat32bppARGB);
    for (int y = 0; y != 2; ++y) for (int x = 0; x != 2; ++x)
        image.SetPixel(x, y, Gdiplus::Color(color));
    image.SetPixel(1, 1, Gdiplus::Color(0x800000ff));
    if (image.Save(path.c_str(), &encoder, nullptr) != Gdiplus::Ok)
        throw std::runtime_error("Authored PNG could not be written");
}
void check_color(Gdiplus::Image& image, Gdiplus::ARGB expected) {
    Gdiplus::Bitmap canvas(2, 2, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&canvas);
        if (graphics.DrawImage(&image, 0, 0, 2, 2) != Gdiplus::Ok)
            throw std::runtime_error("Retained image cannot be drawn");
    }
    Gdiplus::Color pixel;
    if (canvas.GetPixel(0, 0, &pixel) != Gdiplus::Ok || pixel.GetValue() != expected)
        throw std::runtime_error("Retained image pixels changed");
    if (canvas.GetPixel(1, 1, &pixel) != Gdiplus::Ok || pixel.GetValue() != 0x800000ff)
        throw std::runtime_error("Retained image transparency changed");
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        GraphicsSession graphics;
        Menu menu{{fs::u8path(argv[1])}};
        const auto directory = menu.store.root / "bin" / "file-select-art";
        fs::create_directories(directory);
        const auto target = directory / "fixture.png", replacement = directory / "fixture.png.new";
        constexpr Gdiplus::ARGB red = 0xffff0000, green = 0xff00ff00;
        write_png(target, red);
        auto first = menu.art("fixture");
        if (!first || first->GetWidth() != 2 || first->GetHeight() != 2)
            throw std::runtime_error("Authored image did not load");
        check_color(*first, red);
        write_png(replacement, green);
        if (!MoveFileExW(replacement.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
            throw std::runtime_error("Displayed artwork blocks atomic replacement: Win32 " + std::to_string(GetLastError()));
        check_color(*first, red);
        auto second = menu.art("fixture");
        if (!second) throw std::runtime_error("Replacement image did not load");
        check_color(*second, green);
        if (!DeleteFileW(target.c_str())) throw std::runtime_error("Displayed artwork blocks removal");
        check_color(*first, red);
        check_color(*second, green);
        if (menu.art("fixture")) throw std::runtime_error("Missing artwork must use its text fallback");
        { std::ofstream bad(target, std::ios::binary); bad << "authored invalid image"; }
        if (menu.art("fixture")) throw std::runtime_error("Invalid artwork must use its text fallback");
        std::cout << "Displayed artwork survives replacement/removal; refreshed pixels and fallbacks are correct.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
'''


def actual_function(source, signature):
    start = source.index(signature)
    end = source.index("\n}", start) + 2 if not signature.startswith("    ") else source.index("\n    }", start) + 6
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    selector = (ROOT / "builder/setup/file_select.cpp").read_text(encoding="utf-8")
    code = FIXTURE.replace("__PICTURE__", actual_function(selector, "std::unique_ptr<Gdiplus::Image> picture_from("))
    code = code.replace("__ART__", actual_function(selector, "    std::unique_ptr<Gdiplus::Image> art("))
    with tempfile.TemporaryDirectory(prefix="hookshot-file-select-art-") as temp:
        source, binary = Path(temp) / "art.cpp", Path(temp) / "art.exe"
        source.write_text(code, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-static", "-Wall", "-Wextra", "-Werror", "-I",
                        str(ROOT / "builder/setup"), str(source), "-lgdiplus", "-lshlwapi", "-lole32",
                        "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary), str(Path(temp) / "game")], check=True, timeout=20)


if __name__ == "__main__":
    main()
