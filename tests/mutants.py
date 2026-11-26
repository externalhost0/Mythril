#!/usr/bin/env python3
"""Put each past sync bug back into the library, rebuild, and require its test case to fail. usage: mutants.py [build dir] [-k name]"""
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UPDATE = "cmdUpdateBufferEXT barriers*"
UPLOAD = "staging upload barriers*"
TRACKER = "render graph buffer tracker orders every hazard*"
PASS = "a buffer declared up to 3 times*"


@dataclass
class Mutant:
    name: str
    file: str
    old: str
    new: str
    test: str
    count: int = 1  # occurrences to replace, 0 replaces all


MUTANTS = [
    Mutant("BDA shader writes not waited on before an update",
           "lib/vkutil.h",
           "{VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, kAnyShaderStage,",
           "{VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kAnyShaderStage,", UPDATE),
    Mutant("ALL_COMMANDS before an update",
           "lib/vkutil.h",
           ".before = {.src = {.stage = consumers.stage, .access = consumers.access & kWriteAccess}, .dst = update},",
           ".before = {.src = {.stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .access = consumers.access & kWriteAccess}, .dst = update},", UPDATE),
    Mutant("update consumers widened with a stage and MEMORY_READ",
           "lib/vkutil.h",
           ".after = {.src = update, .dst = consumers},",
           ".after = {.src = update, .dst = {.stage = consumers.stage | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, .access = consumers.access | VK_ACCESS_2_MEMORY_READ_BIT}},",
           UPDATE),
    Mutant("later copies not ordered after an upload",
           "lib/vkutil.h",
           "{VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, BufferUseGate::None},",
           "", UPLOAD),
    Mutant("no barrier before a staging copy",
           "lib/vkutil.h",
           ".before = {.src = {.stage = consumers.stage, .access = consumers.access & kWriteAccess}, .dst = copy},",
           ".before = {},", UPLOAD),
    Mutant("shader reads not made visible after an upload",
           "lib/vkutil.h",
           ".after = {.src = copy, .dst = consumers},",
           ".after = {.src = copy, .dst = {.stage = consumers.stage, .access = consumers.access & ~VK_ACCESS_2_SHADER_STORAGE_READ_BIT}},",
           UPLOAD),
    Mutant("tracker drops readers since the last write",
           "lib/vkutil.h",
           "const StageAccess src = tracker.readStages ? StageAccess{.stage = tracker.readStages, .access = 0} : tracker.write;",
           "const StageAccess src = tracker.write;", TRACKER),
    Mutant("tracker keeps visibility as one stage union for all access bits",
           "lib/vkutil.h",
           "tracker.visible[std::countr_zero(bits)]",
           "tracker.visible[0]", TRACKER, count=0),
    Mutant("one barrier per declaration in a pass",
           "lib/RenderGraphInternal.h",
           "if (it == barriers.end()) {",
           "if (true) {", PASS),
]


def build(build_dir: Path) -> bool:
    return subprocess.run(["cmake", "--build", str(build_dir), "--target", "mythril_tests"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def run(build_dir: Path, test: str) -> bool:
    return subprocess.run([str(build_dir / "mythril_tests"), f"-tc={test}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def restore(path: Path, text: str) -> None:
    path.write_text(text)
    # a restore in the same mtime tick as the mutant build can skip the rebuild
    now = time.time() + 1
    os.utime(path, (now, now))


def main() -> int:
    args = sys.argv[1:]
    only = None
    if "-k" in args:
        i = args.index("-k")
        only = args[i + 1]
        del args[i:i + 2]
    build_dir = (ROOT / (args[0] if args else "build/debug")).resolve()
    mutants = [m for m in MUTANTS if only is None or only in m.name]

    survived = []
    for m in mutants:
        path = ROOT / m.file
        original = path.read_text()
        found = original.count(m.old)
        if found == 0 or (m.count == 1 and found != 1):
            print(f"STALE   {m.name}: expected text found {found} times in {m.file}, update the mutant")
            survived.append(m.name)
            continue
        try:
            path.write_text(original.replace(m.old, m.new, -1 if m.count == 0 else m.count))
            if not build(build_dir):
                print(f"NOBUILD {m.name}: the mutant does not compile, fix it")
                survived.append(m.name)
            elif run(build_dir, m.test):
                print(f"SURVIVED {m.name}: '{m.test}' still passes")
                survived.append(m.name)
            else:
                print(f"killed  {m.name}")
        finally:
            restore(path, original)

    if not build(build_dir) or not run(build_dir, "*"):
        print("restored tree does not build or pass, check git diff")
        return 1
    print(f"{len(mutants) - len(survived)}/{len(mutants)} mutants killed")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
