"""Builds PufferLib's _C with the Crimsonland env linked in (float32, for the PyTorch backend).

Run with .venv's Python after `just puffer-setup`. Targets the RTX 5070 (sm_120). The env (env/*.cpp, core/world.cpp)
is compiled with clang++ and linked straight into _C next to PufferLib's binding of it (puffer/binding.c).
"""
import os
import shutil
import subprocess
import sysconfig
from pathlib import Path

import numpy
import pybind11

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / "puffer"
REVISION = "42f70d6932c30ac977736f861006809c50168ba9"
SOURCE = ROOT / "upstream" / f"PufferLib-{REVISION}"
OUT = ROOT / "build" / "puffer"
CORE = ROOT / "upstream/crimson/crimson-core"
OUT.mkdir(parents=True, exist_ok=True)


def run(*cmd, **kw):
    subprocess.run([str(c) for c in cmd], check=True, **kw)


# PufferLib's sources, with the teardown fix for runs without CUDA graphs, in a scratch copy.
overlay = SOURCE / "src-crimson"  # beside src/: the CUDA sources include ../ocean/...
shutil.copytree(SOURCE / "src", overlay, dirs_exist_ok=True)
if "if (pufferl.train_cudagraph)" not in (overlay / "pufferlib.cu").read_text():
    run("patch", "-p1", "-i", HERE / "pufferlib-close.patch", cwd=overlay)

core_so = ROOT / "build/core/libcrimson_core.so"
cxx = ["clang++", "-O2", "-g", "-std=c++17", "-fPIC", "-fms-extensions", "-fopenmp", "-I", ROOT / "core",
       "-I", ROOT / "env", "-I", ROOT / "tas", "-I", CORE / "host", "-I", ROOT / "upstream/crimson/third_party/headers",
       "-I", CORE / "build/native/include"]
objects = []
for src in [ROOT / "core/world.cpp", ROOT / "env/env.cpp", ROOT / "env/capi.cpp"]:
    obj = OUT / (src.stem + ".o")
    run(*cxx, "-c", src, "-o", obj)
    objects.append(obj)
run("cc", "-O2", "-fPIC", "-fopenmp", "-DPRECISION_FLOAT", f'-DCRIMSON_CORE_SO_DEFAULT="{core_so}"',
    "-I", overlay, "-I", ROOT / "env", "-c", HERE / "binding.c", "-o", OUT / "binding.o")

nvidia = Path(sysconfig.get_path("purelib")) / "nvidia"
cuda = nvidia / "cu13"
includes = [overlay, Path(sysconfig.get_path("include")), Path(pybind11.get_include()), Path(numpy.get_include()),
            cuda / "include", nvidia / "cudnn/include", nvidia / "nccl/include"]
cuda_obj = OUT / "bindings.o"
if not cuda_obj.exists():  # PufferLib's trainer; only the pinned sources feed it
    run(cuda / "bin/nvcc", "-O3", "-std=c++17", "-arch=sm_120", "-ccbin", "g++-15", "-Xcompiler=-fPIC,-fopenmp",
        "-DPRECISION_FLOAT", "-DENV_NAME=crimson", "-DOBS_TENSOR_T=FloatTensor",
        *["-I" + str(p) for p in includes], "-c", overlay / "bindings.cu", "-o", cuda_obj)

libraries = [cuda / "lib", nvidia / "cudnn/lib", nvidia / "nccl/lib"]
names = ["libcudart.so.13", "libcublas.so.13", "libcublasLt.so.13", "libcusolver.so.12", "libcurand.so.10",
         "libcudnn.so.9", "libnccl.so.2"]
links = [str(next(p / n for p in libraries if (p / n).exists())) for n in names]
target = SOURCE / "pufferlib" / ("_C" + sysconfig.get_config_var("EXT_SUFFIX"))
tmp = target.with_name(target.name + ".tmp")
# Link to a temporary name and rename: a running trainer keeps the old inode mapped.
run("c++", "-shared", "-fPIC", "-fopenmp", cuda_obj, OUT / "binding.o", *objects, *links,
    *["-Wl,-rpath," + str(p) for p in libraries], "-l:libnvidia-ml.so.1", "-ldl", "-lm", "-lpthread", "-o", tmp)
os.replace(tmp, target)
shutil.copyfile(HERE / "crimson.ini", SOURCE / "config/crimson.ini")
print(target)
