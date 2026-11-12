"""A slow capture allocation must not hold up returned buffers or their reuse."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def block(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    source = (args.source / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    actual = "\n".join(block(source, signature) for signature in (
        "void cap_buf_free(", "uint64_t cap_buf_size(", "bool cap_buf_get("))
    template = (ROOT / "tests/capture_buffer_lock_test.cpp").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="capture-buffer-lock-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(template.replace("/* ACTUAL_FUNCTIONS */", actual), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                        str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == "__main__":
    main()
