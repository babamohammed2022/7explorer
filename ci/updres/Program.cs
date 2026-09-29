// UpdateResource behavior probes against the real Win7 explorer.exe.
// Compiled in CI via Add-Type (no project file needed).
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class RU {
    public static readonly List<string> Log = new List<string>();
    static void Out(string s) { Log.Add(s); }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr BeginUpdateResource(string file, bool del);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool UpdateResource(IntPtr h, string type, IntPtr name,
        ushort lang, byte[] data, uint cb);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool UpdateResource(IntPtr h, IntPtr type, IntPtr name,
        ushort lang, byte[] data, uint cb);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool EndUpdateResource(IntPtr h, bool discard);

    static string pristine = @"exp\pristine.exe";

    static byte[] FakeStringBlock(int bytes) {
        byte[] p = new byte[bytes];
        for (int i = 0; i < p.Length; i += 2) { p[i] = 0x41; p[i + 1] = 0; }
        return p;
    }

    static void WriteStrings(string file, ushort lang, string tag) {
        IntPtr h = BeginUpdateResource(file, false);
        if (h == IntPtr.Zero) {
            Out(tag + ": BeginUpdateResource failed gle="
                              + Marshal.GetLastWin32Error());
            return;
        }
        byte[] payload = FakeStringBlock(376);
        bool ok = UpdateResource(h, new IntPtr(6), new IntPtr(337), lang,
                                 payload, (uint)payload.Length);
        int gle = Marshal.GetLastWin32Error();
        bool okE = EndUpdateResource(h, !ok);
        int gleE = Marshal.GetLastWin32Error();
        Out(tag + ": UpdateResource(STRING 337, lang 0x"
            + lang.ToString("X4") + ") ok=" + ok + " gle=" + gle
            + " ; End ok=" + okE + " gle=" + gleE);
    }

    static void CopyPristine(string f) {
        System.IO.File.Copy(pristine, f, true);
    }

    public static void V1AndV2() {
        CopyPristine(@"exp\v1.exe");
        WriteStrings(@"exp\v1.exe", 0x0409,
                     "V1 write-strings-only (en-US)");
        CopyPristine(@"exp\v2.exe");
        WriteStrings(@"exp\v2.exe", 0x0407,
                     "V2 write-strings-only (de-DE)");
    }

    public static void V3() {
        // commit 1: delete the MUI type alone
        CopyPristine(@"exp\v3.exe");
        IntPtr h = BeginUpdateResource(@"exp\v3.exe", false);
        bool okD = UpdateResource(h, "MUI", new IntPtr(1), 0, null, 0);
        int gleD = Marshal.GetLastWin32Error();
        bool okE1 = EndUpdateResource(h, !okD);
        int gleE1 = Marshal.GetLastWin32Error();
        Out("V3a delete-MUI commit: ok=" + okD + " gle=" + gleD
                          + " ; End ok=" + okE1 + " gle=" + gleE1);
        // commit 2: now write strings into the un-marked file
        WriteStrings(@"exp\v3.exe", 0x0409,
                     "V3b write-strings AFTER MUI deletion");
    }

    public static void V4() {
        // single transaction: delete MUI then write strings, discard at end
        CopyPristine(@"exp\v4.exe");
        IntPtr h = BeginUpdateResource(@"exp\v4.exe", false);
        bool okD = UpdateResource(h, "MUI", new IntPtr(1), 0, null, 0);
        int gleD = Marshal.GetLastWin32Error();
        byte[] payload = FakeStringBlock(376);
        bool okW = UpdateResource(h, new IntPtr(6), new IntPtr(337), 0x0409,
                                  payload, (uint)payload.Length);
        int gleW = Marshal.GetLastWin32Error();
        bool okE = EndUpdateResource(h, true);
        Out("V4 same-tx: delMUI ok=" + okD + " gle=" + gleD
                          + " ; writeStrings ok=" + okW + " gle=" + gleW
                          + " ; End(discard)=" + okE);
    }

    public static void V5() {
        // full rewrite: delete all existing resources, write ours in
        CopyPristine(@"exp\v5.exe");
        IntPtr h = BeginUpdateResource(@"exp\v5.exe", true);
        byte[] payload = FakeStringBlock(376);
        bool okW = UpdateResource(h, new IntPtr(6), new IntPtr(337), 0x0409,
                                  payload, (uint)payload.Length);
        int gleW = Marshal.GetLastWin32Error();
        bool okE = EndUpdateResource(h, false);
        int gleE = Marshal.GetLastWin32Error();
        Out("V5 deleteExisting=true: write ok=" + okW
                          + " gle=" + gleW + " ; End ok=" + okE
                          + " gle=" + gleE);
    }
}
