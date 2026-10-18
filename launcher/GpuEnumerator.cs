using System.Runtime.InteropServices;

namespace SupermanReturnsLauncher;

public sealed record GpuAdapter(int Index, string Name, string Luid = "") { public override string ToString() => Index < 0 ? L.T("AutoGpu") : Name; }
internal static class GpuEnumerator
{
    [DllImport("dxgi.dll", ExactSpelling = true)]
    private static extern int CreateDXGIFactory1(ref Guid iid, out nint factory);
    [DllImport("d3d12.dll", ExactSpelling = true)]
    private static extern int D3D12CreateDevice(nint adapter, uint featureLevel, ref Guid iid, nint device);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int EnumAdapters(nint factory, uint index, out nint adapter);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetDesc(nint adapter, out AdapterDescription description);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct AdapterDescription
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description;
        public uint VendorId, DeviceId, SubsystemId, Revision;
        public nuint DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
        public uint LuidLow; public int LuidHigh; public uint Flags;
    }
    private static T Method<T>(nint instance, int slot) where T : Delegate =>
        Marshal.GetDelegateForFunctionPointer<T>(Marshal.ReadIntPtr(Marshal.ReadIntPtr(instance), slot * nint.Size));

    public static IReadOnlyList<GpuAdapter> GetAdapters()
    {
        var result = new List<GpuAdapter> { new(-1, "Auto") };
        nint factory = 0;
        try
        {
            var iid = new Guid("770aae78-f26f-4dba-a829-253c83d1b387"); // IDXGIFactory1
            if (CreateDXGIFactory1(ref iid, out factory) < 0) return result;
            var enumerate = Method<EnumAdapters>(factory, 12);
            for (uint i = 0; ; i++)
            {
                if (enumerate(factory, i, out var adapter) < 0) break;
                try
                {
                    var deviceId = new Guid("189819f1-1db6-4b57-be54-1821339b85f7"); // ID3D12Device
                    if (Method<GetDesc>(adapter, 10)(adapter, out var desc) >= 0 && (desc.Flags & 2) == 0
                        && D3D12CreateDevice(adapter, 0xb000, ref deviceId, 0) >= 0)
                        result.Add(new((int)i, desc.Description.Trim(), $"{unchecked((uint)desc.LuidHigh):X8}{desc.LuidLow:X8}"));
                }
                finally { Marshal.Release(adapter); }
            }
        }
        catch (DllNotFoundException) { }
        catch (EntryPointNotFoundException) { }
        finally { if (factory != 0) Marshal.Release(factory); }
        return result;
    }
}
