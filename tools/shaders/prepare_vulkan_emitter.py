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
 patches=[root/'tools/shaders/xenosrecomp/patches'/name for name in ('0011-vulkan-buffer-bindings.patch','0014-stacked-tfetch3d.patch')]
 args=['git','apply','--directory',(target.relative_to(root)/'src').as_posix()]
 # fetch_xenosrecomp.py applies every patch to the source and stamps .sr_pin with
 # their names and fingerprint; later patches change the context of earlier ones,
 # so a per-patch reverse check cannot tell that they are already there.
 from fetch_xenosrecomp import patch_fingerprint
 stamp=source/'src/.sr_pin'
 lines=stamp.read_text().splitlines() if stamp.is_file() else []
 series=sorted((root/'tools/shaders/xenosrecomp/patches').glob('*.patch'))
 applied=bool(lines) and lines[-1]==patch_fingerprint(series) and all(p.name in lines for p in patches)
 for patch in ([] if applied else patches):
  check=subprocess.run([*args,'--check',str(patch)],cwd=root,capture_output=True,text=True)
  if check.returncode:
   reverse=subprocess.run([*args,'--reverse','--check',str(patch)],cwd=root,capture_output=True,text=True)
   if reverse.returncode:raise RuntimeError('ABI patch '+patch.name+' conflicts with copied emitter source; original source preserved: '+check.stderr)
  else:subprocess.run([*args,str(patch)],cwd=root,check=True)
 fingerprint=hashlib.sha256(b''.join(p.read_bytes() for p in patches)).hexdigest()
 (target/'abi-patch.sha256').write_text(fingerprint,encoding='ascii')
 return target
if __name__=='__main__':
 try:print(prepare())
 except (OSError,RuntimeError,subprocess.CalledProcessError) as e:print(str(e),file=sys.stderr);sys.exit(1)
