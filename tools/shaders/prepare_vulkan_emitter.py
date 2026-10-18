"""Prepare an isolated emitter tree; never rewrite the existing DXIL toolchain."""
from pathlib import Path
import argparse,hashlib,shutil,subprocess,sys
ROOT=Path(__file__).resolve().parents[2]
PIN='339af41df2c23dbe3256c1c377716b81a0e0fe6b'
def prepare(root=ROOT):
 source=root/'.tools/xenosrecomp';target=root/'build/vulkan-m2/emitter-tree'
 current=subprocess.run(['git','-C',str(source/'src'),'rev-parse','HEAD'],capture_output=True,text=True)
 if current.returncode or current.stdout.strip()!=PIN:raise RuntimeError('Unexpected emitter source revision; source preserved')
 if not (source/'src/XenosRecomp/shader_recompiler.cpp').is_file():raise RuntimeError('Missing emitter source')
 shutil.copytree(source/'src/XenosRecomp',target/'src/XenosRecomp',dirs_exist_ok=True)
 for name in ('fmt','xxHash'):
  shutil.copytree(source/'deps'/name,target/'deps'/name,dirs_exist_ok=True,ignore=shutil.ignore_patterns('.git','test','tests','doc','docs','build'))
 patch=root/'tools/shaders/xenosrecomp/patches/0011-vulkan-buffer-bindings.patch'
 args=['git','apply','--directory',(target.relative_to(root)/'src').as_posix()]
 check=subprocess.run([*args,'--check',str(patch)],cwd=root,capture_output=True,text=True)
 if check.returncode:
  reverse=subprocess.run([*args,'--reverse','--check',str(patch)],cwd=root,capture_output=True,text=True)
  if reverse.returncode:raise RuntimeError('ABI patch conflicts with copied emitter source; original source preserved: '+check.stderr)
 else:subprocess.run([*args,str(patch)],cwd=root,check=True)
 fingerprint=hashlib.sha256(patch.read_bytes()).hexdigest()
 (target/'abi-patch.sha256').write_text(fingerprint,encoding='ascii')
 return target
if __name__=='__main__':
 try:print(prepare())
 except (OSError,RuntimeError,subprocess.CalledProcessError) as e:print(str(e),file=sys.stderr);sys.exit(1)
