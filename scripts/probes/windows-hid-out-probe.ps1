param(
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class NxHidOutProbe
{
    [StructLayout(LayoutKind.Sequential)]
    private struct SP_DEVICE_INTERFACE_DATA
    {
        public int cbSize;
        public Guid InterfaceClassGuid;
        public int Flags;
        public IntPtr Reserved;
    }

    private const uint DIGCF_PRESENT = 0x2;
    private const uint DIGCF_DEVICEINTERFACE = 0x10;
    private const uint GENERIC_READ = 0x80000000;
    private const uint GENERIC_WRITE = 0x40000000;
    private const uint FILE_SHARE_READ = 0x1;
    private const uint FILE_SHARE_WRITE = 0x2;
    private const uint OPEN_EXISTING = 3;

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid, IntPtr enumerator,
        IntPtr hwndParent, uint flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiEnumDeviceInterfaces(IntPtr deviceInfoSet, IntPtr deviceInfoData,
        ref Guid interfaceClassGuid, uint memberIndex, ref SP_DEVICE_INTERFACE_DATA interfaceData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr deviceInfoSet,
        ref SP_DEVICE_INTERFACE_DATA interfaceData, IntPtr detailData, uint detailDataSize,
        out uint requiredSize, IntPtr deviceInfoData);

    [DllImport("setupapi.dll")]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateFile(string fileName, uint desiredAccess, uint shareMode,
        IntPtr securityAttributes, uint creationDisposition, uint flagsAndAttributes,
        IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteFile(IntPtr file, byte[] buffer, uint bytesToWrite,
        out uint bytesWritten, IntPtr overlapped);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);

    public static string TryWrite()
    {
        Guid hidGuid = new Guid("4D1E55B2-F16F-11CF-88CB-001111000030");
        IntPtr set = SetupDiGetClassDevs(ref hidGuid, IntPtr.Zero, IntPtr.Zero,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == new IntPtr(-1))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "SetupDiGetClassDevs");

        try
        {
            for (uint index = 0; ; index++)
            {
                SP_DEVICE_INTERFACE_DATA data = new SP_DEVICE_INTERFACE_DATA();
                data.cbSize = Marshal.SizeOf(data);
                if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref hidGuid, index, ref data))
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error == 259) // ERROR_NO_MORE_ITEMS
                        break;
                    throw new Win32Exception(error, "SetupDiEnumDeviceInterfaces");
                }

                uint needed;
                SetupDiGetDeviceInterfaceDetail(set, ref data, IntPtr.Zero, 0, out needed, IntPtr.Zero);
                IntPtr detail = Marshal.AllocHGlobal((int)needed);
                try
                {
                    Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
                    if (!SetupDiGetDeviceInterfaceDetail(set, ref data, detail, needed,
                        out needed, IntPtr.Zero))
                        throw new Win32Exception(Marshal.GetLastWin32Error(),
                            "SetupDiGetDeviceInterfaceDetail");
                    string path = Marshal.PtrToStringUni(IntPtr.Add(detail, 4));
                    if (path == null || path.IndexOf("vid_18d1&pid_d002",
                        StringComparison.OrdinalIgnoreCase) < 0)
                        continue;

                    IntPtr handle = CreateFile(path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
                    if (handle == new IntPtr(-1))
                        return "found=true open=false error=" + Marshal.GetLastWin32Error() +
                            " path=" + path;
                    try
                    {
                        // Windows HID writes include a leading zero report-ID byte when the
                        // descriptor has no report IDs. The 32 following bytes travel over ep2.
                        byte[] report = new byte[33];
                        for (int i = 1; i < report.Length; i++)
                            report[i] = (byte)(0xA0 + ((i - 1) & 0x0F));
                        uint written;
                        bool ok = WriteFile(handle, report, (uint)report.Length, out written, IntPtr.Zero);
                        return "found=true open=true write=" + ok + " bytes=" + written +
                            " error=" + (ok ? 0 : Marshal.GetLastWin32Error()) + " path=" + path;
                    }
                    finally { CloseHandle(handle); }
                }
                finally { Marshal.FreeHGlobal(detail); }
            }
        }
        finally { SetupDiDestroyDeviceInfoList(set); }
        return "found=false";
    }
}
'@

$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
do {
    $result = [NxHidOutProbe]::TryWrite()
    if ($result -notmatch '^found=false') {
        $result
        exit ($(if ($result -match 'write=True') { 0 } else { 2 }))
    }
    Start-Sleep -Milliseconds 200
} while ([DateTime]::UtcNow -lt $deadline)

'found=false timeout=true'
exit 1

