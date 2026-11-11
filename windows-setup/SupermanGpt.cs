// Partition-table planner for the Lumia 730 internal Android install. A port of superman-gpt.c ("plan"):
// only Windows Phone's Data partition is re-used; every other entry stays byte for byte.
//   entry 27 "Data" -> "cache" (64 MiB, boot), entry 30 -> "android_system" (2 GiB), entry 31 -> "android_data" (rest)
// primary = LBA 0..33 (protective MBR, header, 32 entry sectors); backup = the last 33 LBAs. 512-byte sectors.
using System;
using System.Text;
public sealed class SupermanGptPlan {
    public string Layout;                  // "windows" (to be converted) or "android" (already converted)
    public byte[] NewPrimary, NewBackup;   // null when the layout is already "android"
    public long Total, BootFirst, BootLast, SystemFirst, SystemLast, DataFirst, DataLast, OldDataFirst, OldDataLast;
}
public static class SupermanGpt {
    const int S = 512; const int Pri = 34 * S; const int Bak = 33 * S; const long MiB = 2048;
    static uint G32(byte[] b, int o) { return (uint)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24); }
    static ulong G64(byte[] b, int o) { return G32(b, o) | (ulong)G32(b, o + 4) << 32; }
    static void P32(byte[] b, int o, uint v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); b[o + 2] = (byte)(v >> 16); b[o + 3] = (byte)(v >> 24); }
    static void P64(byte[] b, int o, ulong v) { P32(b, o, (uint)v); P32(b, o + 4, (uint)(v >> 32)); }
    static uint Crc32(byte[] b, int o, int n) {
        uint c = 0xffffffffu;
        for (int i = 0; i < n; i++) { c ^= b[o + i]; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? (c >> 1) ^ 0xedb88320u : c >> 1; }
        return ~c;
    }
    static bool Same(byte[] a, int ao, byte[] b, int bo, int n) { for (int i = 0; i < n; i++) if (a[ao + i] != b[bo + i]) return false; return true; }
    // GUID bytes in a GPT are little-endian in the first three fields; RFC 4122 is big-endian. The swap is its own inverse.
    static void LeBe(byte[] i, int io, byte[] o, int oo) {
        o[oo] = i[io + 3]; o[oo + 1] = i[io + 2]; o[oo + 2] = i[io + 1]; o[oo + 3] = i[io]; o[oo + 4] = i[io + 5]; o[oo + 5] = i[io + 4];
        o[oo + 6] = i[io + 7]; o[oo + 7] = i[io + 6]; Array.Copy(i, io + 8, o, oo + 8, 8);
    }
    static void Uuid5(byte[] nsLe, int nsOff, string name, byte[] dst, int dOff) {
        byte[] nm = Encoding.ASCII.GetBytes(name); byte[] buf = new byte[16 + nm.Length];
        LeBe(nsLe, nsOff, buf, 0); Array.Copy(nm, 0, buf, 16, nm.Length);
        byte[] dig; using (var sha = System.Security.Cryptography.SHA1.Create()) dig = sha.ComputeHash(buf);
        byte[] be = new byte[16]; Array.Copy(dig, be, 16);
        be[6] = (byte)((be[6] & 0x0f) | 0x50); be[8] = (byte)((be[8] & 0x3f) | 0x80);
        LeBe(be, 0, dst, dOff);
    }
    sealed class Hdr { public ulong My, Alt, First, Last, Elba; public uint Count, Rev; public byte[] Disk = new byte[16]; }
    static Hdr Parse(byte[] b, int h, int ents) {
        if (Encoding.ASCII.GetString(b, h, 8) != "EFI PART" || G32(b, h + 12) != 92 || G32(b, h + 84) != 128) return null;
        byte[] tmp = new byte[92]; Array.Copy(b, h, tmp, 0, 92); P32(tmp, 16, 0);
        if (Crc32(tmp, 0, 92) != G32(b, h + 16)) return null;
        Hdr o = new Hdr(); o.Rev = G32(b, h + 8); o.My = G64(b, h + 24); o.Alt = G64(b, h + 32); o.First = G64(b, h + 40); o.Last = G64(b, h + 48);
        Array.Copy(b, h + 56, o.Disk, 0, 16); o.Elba = G64(b, h + 72); o.Count = G32(b, h + 80);
        if (o.Count == 0 || o.Count > 128) return null;
        if (Crc32(b, ents, (int)o.Count * 128) != G32(b, h + 88)) return null;
        return o;
    }
    static int Ent(int idx) { return 2 * S + (idx - 1) * 128; }               // offset of entry idx (1-based) in a primary buffer
    static bool IsEmpty(byte[] b, int e) { for (int i = 0; i < 16; i++) if (b[e + i] != 0) return false; return true; }
    static string Name(byte[] b, int e) {
        var sb = new StringBuilder();
        for (int i = 0; i < 36; i++) { int c = b[e + 56 + 2 * i] | b[e + 57 + 2 * i] << 8; if (c == 0) break; sb.Append((char)c); }
        return sb.ToString();
    }
    static void PutName(byte[] b, int e, string s) { Array.Clear(b, e + 56, 72); for (int i = 0; i < s.Length; i++) b[e + 56 + 2 * i] = (byte)s[i]; }
    static Hdr Check(byte[] p, byte[] k, long total) {
        if (p == null || k == null || p.Length != Pri || k.Length != Bak) throw new Exception("partition table buffers have the wrong size");
        Hdr hp = Parse(p, S, 2 * S); if (hp == null) throw new Exception("primary GPT header or entries invalid");
        Hdr hb = Parse(k, 32 * S, 0); if (hb == null) throw new Exception("backup GPT header or entries invalid");
        if (hp.My != 1 || hp.Alt != (ulong)total - 1 || hp.Elba != 2) throw new Exception("primary GPT not where expected");
        if (hb.My != (ulong)total - 1 || hb.Alt != 1 || hb.Elba != (ulong)total - 33) throw new Exception("backup GPT not where expected");
        if (!Same(p, 2 * S, k, 0, 32 * S) || !Same(hp.Disk, 0, hb.Disk, 0, 16) || hp.Count != hb.Count) throw new Exception("primary and backup GPT differ");
        if (p[510] != 0x55 || p[511] != 0xaa || p[450] != 0xee) throw new Exception("no protective MBR");
        if (hp.Count < 31) throw new Exception("partition table has fewer than 31 entries");
        return hp;
    }
    static string LayoutOf(byte[] p) {
        if (Name(p, Ent(26)) == "MainOS" && Name(p, Ent(27)) == "Data" && IsEmpty(p, Ent(30)) && IsEmpty(p, Ent(31))) return "windows";
        if (Name(p, Ent(26)) == "MainOS" && Name(p, Ent(27)) == "cache" && Name(p, Ent(30)) == "android_system" && Name(p, Ent(31)) == "android_data") return "android";
        return "other";
    }
    static void BuildHdr(byte[] dst, int o, Hdr h, byte[] ents, int eo, ulong my, ulong alt, ulong elba) {
        Array.Clear(dst, o, S);
        Array.Copy(Encoding.ASCII.GetBytes("EFI PART"), 0, dst, o, 8); P32(dst, o + 8, h.Rev); P32(dst, o + 12, 92);
        P64(dst, o + 24, my); P64(dst, o + 32, alt); P64(dst, o + 40, h.First); P64(dst, o + 48, h.Last);
        Array.Copy(h.Disk, 0, dst, o + 56, 16); P64(dst, o + 72, elba); P32(dst, o + 80, h.Count); P32(dst, o + 84, 128);
        P32(dst, o + 88, Crc32(ents, eo, (int)h.Count * 128));
        P32(dst, o + 16, Crc32(dst, o, 92));
    }
    // First and last sector of the partition with this name, or null.
    public static long[] Find(byte[] primary, string name) {
        Hdr h = Parse(primary, S, 2 * S); if (h == null) throw new Exception("primary GPT header or entries invalid");
        for (int i = 1; i <= (int)h.Count; i++) { int e = Ent(i); if (!IsEmpty(primary, e) && Name(primary, e) == name) return new long[] { (long)G64(primary, e + 32), (long)G64(primary, e + 40) }; }
        return null;
    }
    public static SupermanGptPlan Plan(byte[] pri, byte[] bak, long total, long minDataSectors) {
        Hdr h = Check(pri, bak, total);
        var r = new SupermanGptPlan(); r.Total = total; r.Layout = LayoutOf(pri);
        if (r.Layout == "android") {
            r.BootFirst = (long)G64(pri, Ent(27) + 32); r.BootLast = (long)G64(pri, Ent(27) + 40);
            r.SystemFirst = (long)G64(pri, Ent(30) + 32); r.SystemLast = (long)G64(pri, Ent(30) + 40);
            r.DataFirst = (long)G64(pri, Ent(31) + 32); r.DataLast = (long)G64(pri, Ent(31) + 40);
            r.OldDataFirst = r.BootFirst; r.OldDataLast = r.DataLast;
            if (r.BootLast - r.BootFirst + 1 != 64 * MiB || r.SystemFirst != r.BootLast + 1 || r.SystemLast - r.SystemFirst + 1 != 2048 * MiB
                || r.DataFirst != r.SystemLast + 1 || r.DataLast - r.DataFirst + 1 < minDataSectors)
                throw new Exception("the Android partitions on this phone are not the ones this tool makes");
            return r;
        }
        if (r.Layout != "windows") throw new Exception("not the Windows Phone layout (MainOS 26, Data 27, 30/31 free)");
        int data = Ent(27);
        ulong dfirst = G64(pri, data + 32), dlast = G64(pri, data + 40);
        if (dlast != h.Last || dfirst % (ulong)MiB != 0) throw new Exception("Data is not the last, 1 MiB-aligned partition");
        for (int i = 1; i <= (int)h.Count; i++) {
            int e = Ent(i); if (i == 27 || IsEmpty(pri, e)) continue;
            if (G64(pri, e + 40) >= dfirst) throw new Exception("another partition lies inside or after Data");
        }
        ulong bfirst = dfirst, blast = bfirst + 64 * (ulong)MiB - 1, sfirst = blast + 1, slast = sfirst + 2048 * (ulong)MiB - 1, afirst = slast + 1, alast = dlast;
        if (alast < afirst + 1024 * (ulong)MiB || alast - afirst + 1 < (ulong)minDataSectors) throw new Exception("Data is too small for the Android layout");
        byte[] linuxFs = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };
        byte[] np = (byte[])pri.Clone();
        int[] idx = { 27, 30, 31 }; string[] names = { "cache", "android_system", "android_data" };
        ulong[] f = { bfirst, sfirst, afirst }, l = { blast, slast, alast };
        for (int i = 0; i < 3; i++) {
            int e = Ent(idx[i]); Array.Clear(np, e, 128); Array.Copy(linuxFs, 0, np, e, 16);
            Uuid5(h.Disk, 0, "superman-android/" + names[i], np, e + 16);
            P64(np, e + 32, f[i]); P64(np, e + 40, l[i]); PutName(np, e, names[i]);
        }
        // Protective MBR: Windows Phone writes start 1 + length = disk size, one sector past the end; the Android
        // bootloader then ignores the whole GPT. Use the UEFI value, min(disk size - 1, 0xffffffff).
        ulong want = (ulong)total - 1 > 0xffffffffUL ? 0xffffffffUL : (ulong)total - 1;
        if (G32(np, 446 + 8) == 1 && G32(np, 446 + 12) > want) P32(np, 446 + 12, (uint)want);
        BuildHdr(np, S, h, np, 2 * S, 1, (ulong)total - 1, 2);
        byte[] nb = new byte[Bak]; Array.Copy(np, 2 * S, nb, 0, 32 * S);
        BuildHdr(nb, 32 * S, h, nb, 0, (ulong)total - 1, 1, (ulong)total - 33);
        // prove the result: valid, Android layout, and only entries 27/30/31 differ
        Hdr nh = Check(np, nb, total);
        if (LayoutOf(np) != "android") throw new Exception("internal error: new table invalid");
        for (int i = 1; i <= (int)h.Count; i++) if (i != 27 && i != 30 && i != 31 && !Same(pri, Ent(i), np, Ent(i), 128)) throw new Exception("internal error: other entry changed");
        if (nh.First != h.First || nh.Last != h.Last || !Same(nh.Disk, 0, h.Disk, 0, 16)) throw new Exception("internal error: header changed");
        r.NewPrimary = np; r.NewBackup = nb;
        r.BootFirst = (long)bfirst; r.BootLast = (long)blast; r.SystemFirst = (long)sfirst; r.SystemLast = (long)slast;
        r.DataFirst = (long)afirst; r.DataLast = (long)alast; r.OldDataFirst = (long)dfirst; r.OldDataLast = (long)dlast;
        return r;
    }
    public static bool IsZero(byte[] b, int n) { for (int i = 0; i < n; i++) if (b[i] != 0) return false; return true; }
    public static bool Equal(byte[] a, byte[] b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return false; return true; }
}
