"""Versioned per-draw Vulkan ABI, independent of DXIL descriptor heaps."""
ABI='sr-vulkan-buffers-v1'
def compile_arguments(stage):
 if stage not in ('vs','ps'): raise ValueError('Only vs/ps stages supported')
 args=['-spirv','-fspv-target-env=vulkan1.1','-D','SR_VULKAN_BUFFERS=1','-HV','2021','-all-resources-bound','-Wno-ignored-attributes','-T',stage+'_6_0']
 if stage=='vs':args.append('-fvk-invert-y')
 return args

def build_contract():
 locations={f'POSITION{i}':i for i in range(5)}
 locations.update(dict(NORMAL0=5,NORMAL1=6,TANGENT0=7,BINORMAL0=8,BLENDINDICES0=9,BLENDWEIGHT0=10,COLOR0=11,COLOR1=12))
 locations.update({f'TEXCOORD{i}':13+i for i in range(16)})
 rows=[(0,0,'storage_buffer',1),(0,1,'storage_buffer',1),(0,2,'storage_buffer',1),(1,0,'sampled_image_2d',32),(1,1,'sampled_image_3d',32),(1,2,'sampled_image_cube',32),(1,3,'sampled_image_2d_array',32),(2,0,'sampler',32),(3,0,'storage_buffer',32)]
 return {'abi':ABI,'version':1,'target':'vulkan1.1','bindings':[dict(set=s,binding=b,type=t,count=n) for s,b,t,n in rows],'vertex_locations':locations,'constant_bytes':4096,'shared_bytes':4096}
