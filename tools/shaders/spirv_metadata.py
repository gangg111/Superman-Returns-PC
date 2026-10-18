"""SPIR-V ABI inspection, not a replacement for spirv-val semantic validation."""
import struct
from vulkan_contract import build_contract

def _string(words):
 data=struct.pack('<'+'I'*len(words),*words)
 if b'\0' not in data:raise ValueError('Unterminated SPIR-V string')
 end=data.index(b'\0');return data[:end].decode('utf-8'),(end+4)//4

def inspect_spirv(data):
 if len(data)<20 or len(data)%4:raise ValueError('Truncated SPIR-V')
 words=struct.unpack('<'+'I'*(len(data)//4),data)
 magic,version,_,bound,reserved=words[:5]
 if magic!=0x07230203 or not 0x10000<=version<=0x10300 or not 0<bound<=1000000 or reserved:raise ValueError('Invalid Vulkan 1.1 SPIR-V header')
 types={};constants={};decorations={};variables={};entries=[];caps=[];extensions=[]
 accesses=[]; aliases={}
 offset=5
 def valid_id(value):
  if not 0<value<bound:raise ValueError('SPIR-V ID exceeds bound')
 while offset<len(words):
  first=words[offset];count=first>>16;op=first&65535
  if not count or offset+count>len(words):raise ValueError('Invalid instruction length')
  a=words[offset+1:offset+count];offset+=count
  minimum={17:1,10:1,15:3,21:3,22:2,23:3,24:3,25:8,26:1,27:2,28:3,29:2,30:1,32:3,43:3,50:3,59:3,71:2}.get(op)
  if minimum is not None and len(a)<minimum:raise ValueError('Truncated instruction operands')
  if op==17:caps.append(a[0])
  elif op==10:extensions.append(_string(a)[0])
  elif op==15:
   name,n=_string(a[2:]);valid_id(a[1]);ids=list(a[2+n:]);[valid_id(i) for i in ids]
   entries.append(dict(model=a[0],name=name,interface=ids))
  elif 19<=op<=33:
   if not a:raise ValueError("Type instruction has no result ID")
   valid_id(a[0]);types[a[0]]=(op,list(a[1:]))
  elif op in (43,50):
   valid_id(a[1]);constants[a[1]]=a[2] if len(a)>2 else 0
  elif op in (65,66):
   if len(a)<4:raise ValueError('Truncated access chain')
   aliases[a[1]]=a[2];accesses.append((a[2],a[3]))
  elif op==83:
   if len(a)<3:raise ValueError('Truncated copy object')
   aliases[a[1]]=a[2]
  elif op==59:
   valid_id(a[0]);valid_id(a[1]);variables[a[1]]=dict(type=a[0],storage=a[2])
  elif op==71:
   valid_id(a[0]);decorations.setdefault(a[0],{})[a[1]]=list(a[2:])
 def type_name(i,seen=None):
  seen=set() if seen is None else seen
  if i in seen:raise ValueError('Recursive interface type')
  seen=seen|{i}
  if i not in types:raise ValueError('Unknown interface type ID')
  op,a=types[i]
  if op==21:return ('int' if a[1] else 'uint')+str(a[0])
  if op==22:return 'float'+str(a[0])
  if op==20:return 'bool'
  if op==23:return type_name(a[0],seen)+'x'+str(a[1])
  if op==24:return type_name(a[0],seen)+'m'+str(a[1])
  if op==28:return type_name(a[0],seen)+'['+str(constants.get(a[1],0))+']'
  if op==32:return type_name(a[1],seen)
  if op==30:return 'struct('+','.join(type_name(x,seen) for x in a)+')'
  return 'type'+str(op)
 dynamic_roots=set()
 for base,index in accesses:
  if index in constants:continue
  seen=set()
  while base in aliases:
   if base in seen:raise ValueError('Cyclic access chain')
   seen.add(base);base=aliases[base]
  dynamic_roots.add(base)
 descriptors=[];inputs=[];outputs=[]
 for vid,v in variables.items():
  deco=decorations.get(vid,{})
  pointer=types.get(v['type'])
  if not pointer or pointer[0]!=32:raise ValueError('Variable missing pointer type')
  tid=pointer[1][1];count=1;runtime=False
  op,a=types.get(tid,(0,[]))
  if op in (28,29):
   runtime=op==29
   count=0 if runtime else constants.get(a[1],0)
   if not runtime and not count:raise ValueError('Invalid descriptor array length')
   tid=a[0];op,a=types.get(tid,(0,[]))
  if 33 in deco or 34 in deco:
   if not deco.get(33) or not deco.get(34):raise ValueError('Incomplete descriptor binding')
   if op==26:kind='sampler'
   elif op==25:kind=('sampled_image_2d_array' if a[1]==1 and a[3]==1 else {1:'sampled_image_2d',2:'sampled_image_3d',3:'sampled_image_cube'}.get(a[1],'unsupported_image')) if a[5]==1 else 'storage_image'
   elif op==30 and v['storage'] in (2,12):kind='storage_buffer' if v['storage']==12 or 3 in decorations.get(tid,{}) else 'uniform_buffer'
   else:kind='unsupported_resource'
   descriptors.append(dict(set=deco[34][0],binding=deco[33][0],type=kind,count=count,runtime_array=runtime,dynamic_indexing=(count!=1 and vid in dynamic_roots)))
  elif v['storage'] in (1,3):
   row={'type':type_name(pointer[1][1])}
   if deco.get(30):row['location']=deco[30][0]
   elif deco.get(11):row['builtin']=deco[11][0]
   else:continue
   (inputs if v['storage']==1 else outputs).append(row)
 return dict(version=version,entrypoints=entries,capabilities=sorted(set(caps)),extensions=sorted(set(extensions)),descriptors=descriptors,inputs=inputs,outputs=outputs)

def contract_errors(metadata):
 errors=[];allowed={(x['set'],x['binding']):x for x in build_contract()['bindings']}
 for cap in metadata['capabilities']:
  if cap in (11,5347):errors.append('physical buffer addresses / int64 violate portable ABI')
  elif cap in (5301,5302,5303,5304,5305,5306,5307,5308,5309,5310,5311,5312):errors.append('Descriptor indexing extension violates portable ABI')
  elif cap not in (0,1,28,29,30,31,32,33,50,51,52):errors.append('Unexpected capability '+str(cap))
 for ext in metadata['extensions']:
  if ext!='SPV_KHR_storage_buffer_storage_class':errors.append('Unexpected extension '+ext)
 for row in metadata['descriptors']:
  expected=allowed.get((row['set'],row['binding']))
  if row['runtime_array']:errors.append('Runtime descriptor array violates portable ABI')
  elif not expected or any(row[k]!=expected[k] for k in ('type','count')):errors.append('Descriptor ABI mismatch '+str(row))
 return errors

def validate_pair(vs,ps):
 errors=[];outputs={r['location']:r for r in vs['outputs'] if 'location' in r}
 for row in ps['inputs']:
  if 'location' in row and (row['location'] not in outputs or outputs[row['location']]['type']!=row['type']):errors.append('Stage location/type mismatch '+str(row['location']))
 descriptors={(r['set'],r['binding']):r for r in vs['descriptors']}
 for row in ps['descriptors']:
  old=descriptors.get((row['set'],row['binding']))
  if old and any(row[k]!=old[k] for k in ('type','count')):errors.append('Conflicting stage descriptor '+str(row))
 return errors
