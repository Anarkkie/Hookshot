"""Both Windows Builders retain bounded precheck tracebacks from a real child.

No game or WPF window is needed: Runner.cs is compiled unchanged with stubs for
unrelated launch settings. The Python check and C# check launch the same authored
make_game.py from a path containing spaces and non-ASCII characters.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

RUN_ALL_ARGS = ["--dotnet", "{dotnet}"]
RUN_ALL_SKIP = {"linux": "Windows Builder precheck; actual child-process fixture exercised on Windows"}
ROOT = Path(__file__).resolve().parents[1]

CHILD = '''import sys
mode = sys.argv[1]
if mode == "long":
    sys.stderr.write("x" * (2 * 1024 * 1024) + "\\n")
elif mode == "lines":
    sys.stderr.write("earlier diagnostic\\n" * 100000)
if mode in ("trace", "long", "lines"):
    raise ModuleNotFoundError("authored missing helper")
if mode in ("empty", "blank"):
    sys.stderr.write(" \\n\\t\\n" if mode == "blank" else "")
    sys.exit(7)
if mode == "store":
    sys.stderr.write("earlier line\\nPython was not found; open the Microsoft Store.\\n  \\n")
    sys.exit(1)
if mode.startswith("structured-"):
    # Fill stderr before stdout: a reader that drains serially deadlocks here.
    sys.stderr.write("x" * (2 * 1024 * 1024) + "\\nignored stderr\\n")
    choice = mode.removeprefix("structured-")
    print({"ok": "@@check ok accepted", "bad": "@@check bad unsupported",
           "error": "@@error authored error"}[choice])
    sys.exit(0 if choice == "ok" else 1)
'''

CSHARP = '''using System.Diagnostics;
using System.Text.Json;
namespace HookshotBuilder;
static class BuildWorkers { public static void Apply(ProcessStartInfo p, int? n) {} }
static class BuildMemory { public static void Apply(ProcessStartInfo p, int? n) {} }
static class BuilderHome { public static void PassTo(ProcessStartInfo p) {} }
static class Program {
    static async Task Main(string[] args) {
        var answer = await Runner.Check(new Tools(args[0], args[1]), new[] { args[2] });
        Console.WriteLine(JsonSerializer.Serialize(new { answer.Ok, answer.Text }));
    }
}
'''


class CheckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="builder-check-", dir=ROOT / "work")
        cls.root = Path(cls.temporary.name)
        cls.tools = cls.root / "tools with spaces café 日本語"
        (cls.tools / "nx2").mkdir(parents=True)
        (cls.tools / "nx2/make_game.py").write_text(CHILD, encoding="utf-8")
        project = cls.root / "fixture"
        project.mkdir()
        shutil.copyfile(ROOT / "builder/HookshotBuilder/Runner.cs", project / "Runner.cs")
        (project / "Program.cs").write_text(CSHARP, encoding="utf-8")
        (project / "NuGet.Config").write_text(
            "<configuration><packageSources><clear/></packageSources></configuration>", encoding="utf-8")
        (project / "Fixture.csproj").write_text('''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework><ImplicitUsings>enable</ImplicitUsings>
<Nullable>enable</Nullable><TreatWarningsAsErrors>true</TreatWarningsAsErrors><NuGetAudit>false</NuGetAudit>
</PropertyGroup></Project>''', encoding="utf-8")
        subprocess.run([ARGS.dotnet, "build", str(project / "Fixture.csproj"), "--configuration", "Release",
                        "--nologo", "--verbosity", "quiet"], check=True, timeout=120)
        cls.dll = project / "bin/Release/net8.0/Fixture.dll"

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def check_both(self, mode):
        commands = {
            "Python": [sys.executable, "-I", "-B", __file__, "--python-child", str(self.tools), mode],
            "WPF": [ARGS.dotnet, str(self.dll), str(self.tools), sys.executable, mode],
        }
        for builder, command in commands.items():
            result = subprocess.run(command, check=True, capture_output=True, text=True, encoding="utf-8", timeout=20)
            yield builder, json.loads(result.stdout)

    def test_traceback_keeps_real_utf8_path_and_exception(self):
        for builder, answer in self.check_both("trace"):
            with self.subTest(builder=builder):
                self.assertFalse(answer["Ok"])
                self.assertIn("Traceback (most recent call last):", answer["Text"])
                self.assertIn(str(self.tools / "nx2/make_game.py"), answer["Text"])
                self.assertIn("ModuleNotFoundError: authored missing helper", answer["Text"])

    def test_large_stderr_keeps_bounded_tail_and_reports_truncation(self):
        for mode in ("long", "lines"):
            for builder, answer in self.check_both(mode):
                with self.subTest(builder=builder):
                    self.assertFalse(answer["Ok"])
                    self.assertLessEqual(len(answer["Text"]), 17000)
                    self.assertIn("earlier stderr omitted", answer["Text"])
                    self.assertIn("Traceback (most recent call last):", answer["Text"])
                    self.assertIn(str(self.tools / "nx2/make_game.py"), answer["Text"])
                    self.assertIn("ModuleNotFoundError: authored missing helper", answer["Text"])

    def test_empty_stderr_reports_exit_code(self):
        for mode in ("empty", "blank"):
            for builder, answer in self.check_both(mode):
                with self.subTest(builder=builder):
                    self.assertFalse(answer["Ok"])
                    self.assertIn("exit code 7", answer["Text"])

    def test_store_guidance_survives_blank_trailing_lines(self):
        for builder, answer in self.check_both("store"):
            with self.subTest(builder=builder):
                self.assertFalse(answer["Ok"])
                self.assertIn("Microsoft Store shortcut", answer["Text"])
                self.assertIn("python.org", answer["Text"])

    def test_structured_results_take_precedence_without_pipe_deadlock(self):
        for mode, expected in (("ok", (True, "accepted")), ("bad", (False, "unsupported")),
                               ("error", (False, "authored error"))):
            for builder, answer in self.check_both("structured-" + mode):
                with self.subTest(builder=builder):
                    self.assertEqual((answer["Ok"], answer["Text"]), expected)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--python-child":
        sys.path.insert(0, str(ROOT / "builder/py"))
        from hookshot_builder import core
        ok, text = core.check(core.Tools(sys.argv[2], sys.executable), [sys.argv[3]])
        print(json.dumps({"Ok": ok, "Text": text}))
    else:
        parser = argparse.ArgumentParser()
        parser.add_argument("--dotnet", default="dotnet")
        ARGS = parser.parse_args()
        (ROOT / "work").mkdir(exist_ok=True)
        unittest.main(argv=[sys.argv[0]])
