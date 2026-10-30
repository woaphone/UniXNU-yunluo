#!/usr/bin/env python3
"""Run scoped source/model regressions in a fresh, retained output directory."""
import argparse,json,os,pathlib,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument("--output",required=True,type=pathlib.Path);a=p.parse_args()
out=a.output.resolve()
if out.exists():raise SystemExit("Refusing an existing output directory")
out.mkdir(parents=True)
env=dict(os.environ,ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
flags=["-O1","-g","-Wall","-Wextra","-fsanitize=address,undefined","-fno-sanitize-recover=all","-fno-omit-frame-pointer"]
tests=[("pmic-state","g++",["-std=c++17","tests/board-key-state.cpp"]),("novatek-protocol-model","gcc",["-std=c11","drivers/NativeNovatekTouch/Test/nvt-protocol.c","drivers/NativeNovatekTouch/NvtProtocol.c"]),("novatek-touch-core-model","gcc",["-std=c11","drivers/NativeNovatekTouch/Test/touch-core.c","drivers/NativeNovatekTouch/NvtTouchCore.c","-lm"])]
results=[]
dirty=subprocess.check_output(["git","-C",str(ROOT),"status","--porcelain","--ignore-submodules=all","--untracked-files=normal"],text=True)
if dirty:raise SystemExit("Refusing qualification from a dirty source tree; commit the tested source first")
revision=subprocess.check_output(["git","-C",str(ROOT),"rev-parse","HEAD"],text=True).strip()
for name,compiler,inputs in tests:
 tool=shutil.which(compiler)
 if not tool:raise SystemExit("Missing host compiler: "+compiler)
 target=out/name;cmd=[tool,*flags,*inputs,"-o",str(target)]
 build=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,env=env)
 run=subprocess.run([str(target)],cwd=ROOT,capture_output=True,text=True,env=env) if build.returncode==0 else None
 report={"name":name,"revision":revision,"compile_command":cmd,"compile_returncode":build.returncode,"compile_output":build.stdout+build.stderr,"run_command":[str(target)],"run_returncode":None if run is None else run.returncode,"run_output":"" if run is None else run.stdout+run.stderr,"scope":"Host source/model only. No physical bus, firmware download, IRQ, HID client, or board boot is qualified."}
 results.append(report)
 with (out/(name+".txt")).open("x") as f:f.write(json.dumps(report,indent=2)+"\n")
 print(name,"compile",build.returncode,"run",report["run_returncode"],flush=True)
 if run:print((run.stdout+run.stderr)[-2500:],flush=True)
with (out/"results.json").open("x") as f:json.dump({"revision":revision,"tests":results},f,indent=2);f.write("\n")
if any(x["compile_returncode"] or x["run_returncode"] for x in results):raise SystemExit(1)
