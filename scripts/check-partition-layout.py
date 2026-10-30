#!/usr/bin/env python3
"""Check source policy and boot-ramdisk separation; never flashes or formats."""
import argparse,json,pathlib,struct,tomllib
ROOT=pathlib.Path(__file__).resolve().parents[1]
def source_policy():
 board=tomllib.loads((ROOT/"board.toml").read_text())
 layout=tomllib.loads((ROOT/"partition-layout.toml").read_text())
 if board.get("schema_version")!=2:raise ValueError("board schema must be 2")
 if layout["boot"]["include_unixnu_payload"] is not False:raise ValueError("UniXNU payload must not be included in boot")
 if layout["boot"]["role"]!="mu-silicium-firmware" or layout["cust"]["role"]!="unixnu-runtime-and-payload":raise ValueError("Partition ownership mismatch")
 if board.get("payload",{}).get("loader")=="firmware":raise ValueError("DarwinBoot file-loading from cust is required; firmware-embedded payload loader is not qualified")
 return layout

def inspect_boot(path):
 size=path.stat().st_size
 with path.open("rb") as f:header=f.read(1648)
 if len(header)<44 or header[:8]!=b"ANDROID!":raise ValueError("Invalid Android boot header")
 version=struct.unpack_from("<I",header,40)[0]
 kernel=struct.unpack_from("<I",header,8)[0]
 if version in (3,4):
  ramdisk=struct.unpack_from("<I",header,12)[0];page=4096
  header_size=struct.unpack_from("<I",header,20)[0]
  if header_size!=(1584 if version==4 else 1580):raise ValueError("Invalid modern boot header size")
 elif version in (0,1,2):
  ramdisk=struct.unpack_from("<I",header,16)[0];page=struct.unpack_from("<I",header,36)[0]
  if page not in (2048,4096,8192,16384):raise ValueError("Unsupported boot page size")
 else:raise ValueError("Unsupported Android boot header version")
 if not kernel or size<page+kernel:raise ValueError("Truncated/empty firmware kernel slot")
 if size>64*1024*1024:raise ValueError("boot exceeds known 64-MiB partition limit")
 if ramdisk:raise ValueError("boot ramdisk is nonempty; cust-separated UniXNU must not be embedded there")
 return {"header_version":version,"kernel_bytes":kernel,"ramdisk_bytes":ramdisk,"scope":"Header/ramdisk split only; firmware FV contents, cust loading and actual boot still need verification"}

def self_test(out):
 if out.exists():raise ValueError("Refusing an existing self-test output")
 out.mkdir(parents=True)
 tests=[]
 for name,ramdisk,kernel,expected in [("split",0,16,True),("payload-in-boot",16,16,False),("empty-kernel",0,0,False),("truncated-kernel",0,8192,False)]:
  header=bytearray(4096);header[:8]=b"ANDROID!"
  struct.pack_into("<4I",header,8,kernel,ramdisk,0,1584);struct.pack_into("<I",header,40,4)
  path=out/(name+".img")
  with path.open("xb") as f:f.write(header+b"x"*16)
  try:inspect_boot(path);accepted=True
  except ValueError:accepted=False
  if accepted!=expected:raise ValueError("Unexpected fixture result: "+name)
  tests.append({"fixture":name,"accepted":accepted,"expected":expected})
 return tests

def main():
 p=argparse.ArgumentParser();p.add_argument("--boot-image",type=pathlib.Path);p.add_argument("--self-test",type=pathlib.Path);p.add_argument("--require-build-ready",action="store_true");a=p.parse_args()
 layout=source_policy();result={"source_policy":"PASS","device_operations":False}
 if a.boot_image:result["boot_header"]=inspect_boot(a.boot_image)
 if a.self_test:result["fixture_tests"]=self_test(a.self_test)
 if a.require_build_ready:
  missing=[k for k in ("partition_geometry_verified","filesystem_verified","firmware_load_path_verified") if layout["cust"].get(k) is not True]
  if missing:raise ValueError("cust build/release blocked: "+", ".join(missing))
 print(json.dumps(result,indent=2))
if __name__=="__main__":
 try:main()
 except (ValueError,KeyError,OSError) as exc:raise SystemExit(str(exc))
