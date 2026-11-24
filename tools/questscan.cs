using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;

// Read-only scan of Skyrim Special Edition's own files (tools/questscan.ps1 says what it writes): the quests of the
// official masters (Skyrim.esm, Update.esm, Dawnguard.esm, HearthFires.esm, Dragonborn.esm: their QUST records, the
// later file's record of a quest replacing the earlier's, as the game loads them) and the compiled scripts (the .pex in
// Skyrim - Misc.bsa, then the loose ones in Data\Scripts, which the game prefers). Written for Windows PowerShell 5.1's
// compiler (C# 5: no string interpolation, no out variables). The sister project's quest scan (the same reader, for
// Fallout 4's files) is where the walk comes from.
public static class SkyrimFiles
{
    // The official masters, in their load order. A quest's file is its index here.
    public static readonly string[] MASTERS = { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" };
    public const string SCRIPTS = "Skyrim - Misc.bsa";

    public static byte[] Inflate(byte[] a_data, int a_offset, int a_length, int a_size)
    {
        using (var ms = new MemoryStream(a_data, a_offset + 2, a_length - 2))
        using (var ds = new DeflateStream(ms, CompressionMode.Decompress))
        {
            var outb = new byte[a_size];
            int read = 0;
            while (read < a_size)
            {
                int n = ds.Read(outb, read, a_size - read);
                if (n <= 0) break;
                read += n;
            }
            return outb;
        }
    }

    // ---------------- ESM: quests ----------------

    // A stage: its index, its INDX flags (0x02 start-up, 0x04 shut-down) and whether a log entry of it completes or
    // fails the quest (QSDT 0x01, 0x02).
    public class Stage { public int Index; public int Flags; public bool Completes; public bool Fails; }

    public class Quest
    {
        public uint Id;            // as the game loads it: the file's index in MASTERS in the top byte
        public int File;           // the master that holds the record the game uses (the last one that has it)
        public string Edid = "";
        public int Type = -1;      // DNAM's quest type: 0 none, 1 main, 2 College, 3 Thieves Guild, 4 Dark Brotherhood,
                                   // 5 Companions, 6 misc, 7 Daedric, 8 side, 9 Civil War, 10 Dawnguard, 11 Dragonborn
        public int Flags;          // DNAM's flags: 0x0001 start game enabled, 0x0100 run once
        public string Event = "";  // ENAM: the Story Manager event that starts it, if any
        public List<Stage> Stages = new List<Stage>();
        public int Objectives;
        // its stage fragments: the stage, the script that holds them and each function's name
        public string FragmentScript = "";
        public Dictionary<int, List<string>> Fragments = new Dictionary<int, List<string>>();
        // the scripts attached to the quest (the fragment script among them), and every object property's form, by name
        public List<string> Scripts = new List<string>();
        public Dictionary<string, uint> Forms = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);

        public List<int> Startup() { return Stages.Where(s => (s.Flags & 0x02) != 0).Select(s => s.Index).ToList(); }
    }

    class Reader
    {
        public byte[] B; public int P; public int End;
        public byte U8() { return B[P++]; }
        public short I16() { var v = BitConverter.ToInt16(B, P); P += 2; return v; }
        public ushort U16() { var v = BitConverter.ToUInt16(B, P); P += 2; return v; }
        public int I32() { var v = BitConverter.ToInt32(B, P); P += 4; return v; }
        public uint U32() { var v = BitConverter.ToUInt32(B, P); P += 4; return v; }
        public string W() { int n = U16(); var s = Encoding.ASCII.GetString(B, P, n); P += n; return s; }
    }

    // An object value in a VMAD: its form ID (objFormat 2: unused, alias, form; 1: form, alias, unused).
    static uint Object(Reader r, int a_format)
    {
        if (a_format == 1) { var form = r.U32(); r.I16(); r.U16(); return form; }
        r.U16(); r.I16(); return r.U32();
    }

    // The scripts of a VMAD block, with their properties; object properties' forms noted by property name.
    static void Scripts(Reader r, int a_version, int a_format, Quest a_quest, bool a_own, Func<uint, uint> a_global)
    {
        int count = r.U16();
        for (int i = 0; i < count; i++)
        {
            var name = r.W();
            if (a_version >= 4) r.U8();
            if (a_own) a_quest.Scripts.Add(name);
            int props = r.U16();
            for (int p = 0; p < props; p++)
            {
                var pname = r.W();
                int type = r.U8();
                if (a_version >= 4) r.U8();
                switch (type)
                {
                    case 1: { var form = a_global(Object(r, a_format)); if (a_own && !a_quest.Forms.ContainsKey(pname)) a_quest.Forms[pname] = form; break; }
                    case 2: r.W(); break;
                    case 3: case 4: r.U32(); break;
                    case 5: r.U8(); break;
                    case 11: { uint n = r.U32(); for (uint k = 0; k < n; k++) Object(r, a_format); break; }
                    case 12: { uint n = r.U32(); for (uint k = 0; k < n; k++) r.W(); break; }
                    case 13: case 14: { uint n = r.U32(); r.P += (int)(4 * n); break; }
                    case 15: { uint n = r.U32(); r.P += (int)n; break; }
                    default: throw new Exception("VMAD property type " + type);
                }
            }
        }
    }

    // A quest's VMAD: its scripts and properties, then its stage fragments (and its aliases' scripts, not read).
    static void Vmad(byte[] a_body, int a_at, int a_size, Quest a_quest, Func<uint, uint> a_global)
    {
        var r = new Reader { B = a_body, P = a_at, End = a_at + a_size };
        int version = r.I16();
        int format = r.I16();
        Scripts(r, version, format, a_quest, true, a_global);
        if (r.P >= r.End) return;
        r.U8();  // unknown (2)
        int frags = r.U16();
        a_quest.FragmentScript = r.W();
        for (int i = 0; i < frags; i++)
        {
            int stage = r.U16();
            r.I16(); r.I32(); r.U8();
            r.W();  // the script (the fragment script)
            var func = r.W();
            List<string> list;
            if (!a_quest.Fragments.TryGetValue(stage, out list)) { list = new List<string>(); a_quest.Fragments[stage] = list; }
            list.Add(func);
        }
    }

    static Quest ReadQuest(byte[] a_body, int a_at, int a_size, Func<uint, uint> a_global)
    {
        var q = new Quest();
        int s = a_at, se = a_at + a_size, big = -1;
        Stage stage = null;
        while (s < se)
        {
            string st = Encoding.ASCII.GetString(a_body, s, 4);
            int ss = BitConverter.ToUInt16(a_body, s + 4);
            if (big >= 0) { ss = big; big = -1; }
            if (st == "XXXX") { big = (int)BitConverter.ToUInt32(a_body, s + 6); s += 10; continue; }
            int d = s + 6;
            if (st == "EDID") q.Edid = Encoding.ASCII.GetString(a_body, d, Math.Max(0, ss - 1));
            else if (st == "DNAM" && ss >= 12) { q.Flags = BitConverter.ToUInt16(a_body, d); q.Type = a_body[d + 8]; }
            else if (st == "ENAM" && ss >= 4) q.Event = Encoding.ASCII.GetString(a_body, d, 4);
            else if (st == "INDX" && ss >= 3) { stage = new Stage { Index = BitConverter.ToUInt16(a_body, d), Flags = a_body[d + 2] }; q.Stages.Add(stage); }
            else if (st == "QSDT" && ss >= 1 && stage != null) { stage.Completes |= (a_body[d] & 0x01) != 0; stage.Fails |= (a_body[d] & 0x02) != 0; }
            else if (st == "QOBJ") { q.Objectives++; stage = null; }
            else if (st == "VMAD") Vmad(a_body, d, ss, q, a_global);
            else if (st == "ANAM") stage = null;  // the aliases begin
            s = d + ss;
        }
        return q;
    }

    // The quests of one master, as the game loads them: a record of a quest from one of its masters is that quest
    // changed (it replaces the earlier file's record).
    public static void Quests(string a_path, int a_file, Dictionary<uint, Quest> a_into)
    {
        Records(a_path, a_file, "QUST", (id, body, at, size, global) =>
        {
            var q = ReadQuest(body, at, size, global);
            q.Id = id;
            q.File = a_file;
            a_into[q.Id] = q;
        });
    }

    // ---------------- ESM: image spaces ----------------

    // An image space modifier (IMAD): whether it plays over a time and ends (DNAM: animatable, then its duration in
    // seconds), or stays on until a script takes it off.
    public class Imod { public uint Id; public string Edid = ""; public bool Animatable; public float Duration; }

    public static void Imods(string a_path, int a_file, Dictionary<uint, Imod> a_into)
    {
        Records(a_path, a_file, "IMAD", (id, body, at, size, global) =>
        {
            var m = new Imod { Id = id };
            for (int s = at, se = at + size; s < se;)
            {
                string st = Encoding.ASCII.GetString(body, s, 4);
                int ss = BitConverter.ToUInt16(body, s + 4);
                int d = s + 6;
                if (st == "EDID") m.Edid = Encoding.ASCII.GetString(body, d, Math.Max(0, ss - 1));
                else if (st == "DNAM" && ss >= 8) { m.Animatable = BitConverter.ToUInt32(body, d) != 0; m.Duration = BitConverter.ToSingle(body, d + 4); }
                s = d + ss;
            }
            a_into[id] = m;
        });
    }

    // The records of one type in one master, as the game loads them: each record's form ID as the game loads it (the
    // file's index in MASTERS in its top byte), its body (inflated if compressed: the body, where it starts, its size)
    // and a_global, which turns a form ID read in that body into the game's. Deleted records are skipped.
    public static void Records(string a_path, int a_file, string a_type, Action<uint, byte[], int, int, Func<uint, uint>> a_each)
    {
        using (var fs = File.OpenRead(a_path))
        using (var br = new BinaryReader(fs))
        {
            var head = br.ReadBytes(24);
            uint hsize = BitConverter.ToUInt32(head, 4);
            var hbody = br.ReadBytes((int)hsize);
            var masters = new List<string>();
            for (int s = 0; s < hbody.Length; s += 6 + BitConverter.ToUInt16(hbody, s + 4))
                if (Encoding.ASCII.GetString(hbody, s, 4) == "MAST") masters.Add(Encoding.ASCII.GetString(hbody, s + 6, BitConverter.ToUInt16(hbody, s + 4) - 1));
            // a form ID in this file as the game loads it: its top byte indexes the file's own masters, then the file itself
            Func<uint, uint> loaded = a_id =>
            {
                int index = (int)(a_id >> 24);
                int file = index < masters.Count ? Array.IndexOf(MASTERS, masters[index]) : a_file;
                return file < 0 ? 0xFFFFFFFF : (a_id & 0x00FFFFFF) | ((uint)file << 24);
            };
            while (fs.Position < fs.Length)
            {
                var gh = br.ReadBytes(24);
                uint gsize = BitConverter.ToUInt32(gh, 4);
                if (Encoding.ASCII.GetString(gh, 8, 4) != a_type) { fs.Position += gsize - 24; continue; }
                var all = br.ReadBytes((int)gsize - 24);
                int p = 0;
                while (p < all.Length)
                {
                    string rt = Encoding.ASCII.GetString(all, p, 4);
                    uint rsize = BitConverter.ToUInt32(all, p + 4);
                    if (rt == "GRUP") { p += 24; continue; }
                    uint flags = BitConverter.ToUInt32(all, p + 8);
                    uint id = BitConverter.ToUInt32(all, p + 12);
                    byte[] body; int bo, bl;
                    if ((flags & 0x40000) != 0) { bl = (int)BitConverter.ToUInt32(all, p + 24); body = Inflate(all, p + 28, (int)rsize - 4, bl); bo = 0; }
                    else { body = all; bo = p + 24; bl = (int)rsize; }
                    p += 24 + (int)rsize;
                    if (rt != a_type || (flags & 0x20) != 0) continue;  // deleted
                    uint gid = loaded(id);
                    if (gid == 0xFFFFFFFF) continue;
                    a_each(gid, body, bo, bl, loaded);
                }
                break;
            }
        }
    }

    // ---------------- BSA (version 105) ----------------

    // The files of a BSA a_want takes (by "folder\name"), read whole. Compressed files (LZ4 in version 105) are not read:
    // the scripts' archive holds none (the scan says so if it ever does).
    public static Dictionary<string, byte[]> Bsa(string a_path, Func<string, bool> a_want, List<string> a_compressed)
    {
        var result = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        using (var fs = File.OpenRead(a_path))
        using (var br = new BinaryReader(fs))
        {
            if (br.ReadUInt32() != 0x00415342) throw new Exception(a_path + ": not a BSA");
            uint version = br.ReadUInt32();
            if (version != 105) throw new Exception(a_path + ": BSA version " + version + " (Special Edition's is 105)");
            br.ReadUInt32();
            uint archive = br.ReadUInt32();
            uint folders = br.ReadUInt32(), files = br.ReadUInt32();
            br.ReadUInt32(); br.ReadUInt32(); br.ReadUInt32();
            bool names = (archive & 0x1) != 0, compressed = (archive & 0x4) != 0, embedded = (archive & 0x100) != 0;
            if (!names || (archive & 0x2) == 0) throw new Exception(a_path + ": no file names");
            var counts = new uint[folders];
            for (int i = 0; i < folders; i++) { br.ReadUInt64(); counts[i] = br.ReadUInt32(); br.ReadUInt32(); br.ReadUInt64(); }
            var folderOf = new List<string>(); var sizes = new List<uint>(); var offsets = new List<uint>();
            for (int i = 0; i < folders; i++)
            {
                int len = br.ReadByte();
                var folder = Encoding.ASCII.GetString(br.ReadBytes(len)).TrimEnd('\0');
                for (int k = 0; k < counts[i]; k++) { br.ReadUInt64(); sizes.Add(br.ReadUInt32()); offsets.Add(br.ReadUInt32()); folderOf.Add(folder); }
            }
            var fileNames = new List<string>();
            var sb = new StringBuilder();
            while (fileNames.Count < files)
            {
                int c = br.ReadByte();
                if (c == 0) { fileNames.Add(sb.ToString()); sb.Clear(); } else sb.Append((char)c);
            }
            for (int i = 0; i < files; i++)
            {
                var full = folderOf[i] + "\\" + fileNames[i];
                if (!a_want(full)) continue;
                uint size = sizes[i];
                bool packed = compressed ^ ((size & 0x40000000) != 0);
                size &= 0x3FFFFFFF;
                fs.Position = offsets[i];
                if (embedded) { int n = br.ReadByte(); br.ReadBytes(n); size -= (uint)(n + 1); }
                if (packed) { a_compressed.Add(full); continue; }
                result[full] = br.ReadBytes((int)size);
            }
        }
        return result;
    }

    // ---------------- PEX (Skyrim's, version 3.2) ----------------

    public class Instr { public int Op; public List<string> Args = new List<string>(); }
    public class Func
    {
        public string Obj; public string State; public string Name; public List<Instr> Code = new List<Instr>();
        // each parameter's and local's type, by name, so a call on one can be followed into its script
        public Dictionary<string, string> Types = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        public List<string> Params = new List<string>();  // in order
    }
    // a script object: its parent, and its properties' and variables' types by name
    public class Obj { public string Name; public string Parent = ""; public Dictionary<string, string> Types = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase); }

    // Skyrim's compiled scripts are big-endian (Fallout 4's are not).
    class PR
    {
        public byte[] B; public int P; public string[] S;
        public byte U8() { return B[P++]; }
        public ushort U16() { var v = (ushort)((B[P] << 8) | B[P + 1]); P += 2; return v; }
        public uint U32() { var v = ((uint)B[P] << 24) | ((uint)B[P + 1] << 16) | ((uint)B[P + 2] << 8) | B[P + 3]; P += 4; return v; }
        public ulong U64() { ulong hi = U32(); return (hi << 32) | U32(); }
        public string W() { int n = U16(); var s = Encoding.ASCII.GetString(B, P, n); P += n; return s; }
        public string Str() { return S[U16()]; }
        public string Var()
        {
            byte t = U8();
            switch (t)
            {
                case 0: return "None";
                case 1: return S[U16()];
                case 2: return "\"" + S[U16()] + "\"";
                case 3: return ((int)U32()).ToString();
                case 4: { var f = BitConverter.ToSingle(BitConverter.GetBytes(U32()), 0); return f.ToString(System.Globalization.CultureInfo.InvariantCulture); }
                case 5: return U8() != 0 ? "True" : "False";
                default: throw new Exception("var type " + t);
            }
        }
    }

    // Each opcode's fixed arguments (0x00 nop to 0x23 array_rfindelement); callmethod, callparent and callstatic have a
    // count and that many more.
    static readonly int[] ARGS = { 0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 1, 2, 2, 3, 2, 3, 1, 3, 3, 3, 2, 2, 3, 3, 4, 4 };
    public static readonly string[] OPS = { "nop", "iadd", "fadd", "isub", "fsub", "imul", "fmul", "idiv", "fdiv", "imod", "not", "ineg", "fneg", "assign", "cast", "cmp_eq", "cmp_lt", "cmp_le", "cmp_gt", "cmp_ge", "jmp", "jmpt", "jmpf", "callmethod", "callparent", "callstatic", "return", "strcat", "propget", "propset", "array_create", "array_length", "array_getelement", "array_setelement", "array_findelement", "array_rfindelement" };
    public const int JMP = 20, JMPT = 21, JMPF = 22, CALLMETHOD = 23, CALLPARENT = 24, CALLSTATIC = 25, RETURN = 26, ASSIGN = 13, CAST = 14, CMP_EQ = 15, NOT = 10;

    static Func ReadFunc(PR r, string a_obj, string a_state, string a_name)
    {
        var f = new Func { Obj = a_obj, State = a_state, Name = a_name };
        r.U16(); r.U16(); r.U32(); r.U8();  // return type, doc, user flags, flags
        int pc = r.U16(); for (int i = 0; i < pc; i++) { var n = r.Str(); f.Types[n] = r.Str(); f.Params.Add(n); }
        int lc = r.U16(); for (int i = 0; i < lc; i++) { var n = r.Str(); f.Types[n] = r.Str(); }
        int ic = r.U16();
        for (int i = 0; i < ic; i++)
        {
            var ins = new Instr { Op = r.U8() };
            if (ins.Op >= ARGS.Length) throw new Exception("opcode " + ins.Op);
            for (int a = 0; a < ARGS[ins.Op]; a++) ins.Args.Add(r.Var());
            if (ins.Op == CALLMETHOD || ins.Op == CALLPARENT || ins.Op == CALLSTATIC)
            {
                int cnt = int.Parse(r.Var());
                for (int a = 0; a < cnt; a++) ins.Args.Add(r.Var());
            }
            f.Code.Add(ins);
        }
        return f;
    }

    public static List<Func> Pex(byte[] a_b, out List<Obj> a_objs)
    {
        a_objs = new List<Obj>();
        var r = new PR { B = a_b };
        if (r.U32() != 0xFA57C0DE) throw new Exception("magic");
        r.U8(); r.U8(); r.U16(); r.U64(); r.W(); r.W(); r.W();
        int sc = r.U16(); r.S = new string[sc]; for (int i = 0; i < sc; i++) r.S[i] = r.W();
        if (r.U8() != 0)
        {
            r.U64();
            int fc = r.U16(); for (int i = 0; i < fc; i++) { r.U16(); r.U16(); r.U16(); r.U8(); int l = r.U16(); r.P += 2 * l; }
        }
        int uf = r.U16(); for (int i = 0; i < uf; i++) { r.U16(); r.U8(); }
        var funcs = new List<Func>();
        int oc = r.U16();
        for (int o = 0; o < oc; o++)
        {
            string oname = r.Str(); r.U32();
            var obj = new Obj { Name = oname, Parent = r.Str() }; a_objs.Add(obj);
            r.U16(); r.U32(); r.U16();  // doc, user flags, auto state
            int vc = r.U16(); for (int v = 0; v < vc; v++) { var vn = r.Str(); obj.Types[vn] = r.Str(); r.U32(); r.Var(); }
            int pcnt = r.U16();
            for (int p = 0; p < pcnt; p++)
            {
                string pn = r.Str(); obj.Types[pn] = r.Str(); r.U16(); r.U32(); byte fl = r.U8();
                if ((fl & 4) != 0) r.U16();
                else { if ((fl & 1) != 0) ReadFunc(r, oname, "", pn + ".get"); if ((fl & 2) != 0) ReadFunc(r, oname, "", pn + ".set"); }
            }
            int stc = r.U16();
            for (int s = 0; s < stc; s++)
            {
                string sn = r.Str(); int fc = r.U16();
                for (int k = 0; k < fc; k++) { string fn = r.Str(); funcs.Add(ReadFunc(r, oname, sn, fn)); }
            }
        }
        return funcs;
    }

    public static string Text(Instr a_i) { return OPS[a_i.Op] + " " + string.Join(", ", a_i.Args.ToArray()); }
}

// Every quest and script of the masters, read once.
public static class Game
{
    public class Script { public SkyrimFiles.Obj Obj; public Dictionary<string, SkyrimFiles.Func> Funcs = new Dictionary<string, SkyrimFiles.Func>(StringComparer.OrdinalIgnoreCase); }

    public static readonly Dictionary<uint, SkyrimFiles.Quest> quests = new Dictionary<uint, SkyrimFiles.Quest>();
    public static readonly Dictionary<string, SkyrimFiles.Quest> byEdid = new Dictionary<string, SkyrimFiles.Quest>(StringComparer.OrdinalIgnoreCase);
    public static readonly Dictionary<string, Script> scripts = new Dictionary<string, Script>(StringComparer.OrdinalIgnoreCase);
    // the quest a script is attached to, when it is attached to one quest only (a template on several says nothing)
    public static readonly Dictionary<string, SkyrimFiles.Quest> questOfScript = new Dictionary<string, SkyrimFiles.Quest>(StringComparer.OrdinalIgnoreCase);
    // the image space modifiers, by form ID and by editor ID
    public static readonly Dictionary<uint, SkyrimFiles.Imod> imods = new Dictionary<uint, SkyrimFiles.Imod>();
    public static readonly Dictionary<string, SkyrimFiles.Imod> imodByEdid = new Dictionary<string, SkyrimFiles.Imod>(StringComparer.OrdinalIgnoreCase);
    public static readonly List<string> notes = new List<string>();

    static void AddScript(string a_name, byte[] a_pex)
    {
        List<SkyrimFiles.Obj> objs; List<SkyrimFiles.Func> funcs;
        try { funcs = SkyrimFiles.Pex(a_pex, out objs); }
        catch (Exception e) { notes.Add("# unread script " + a_name + ": " + e.Message); return; }
        foreach (var o in objs) scripts[o.Name] = new Script { Obj = o };
        foreach (var f in funcs) { Script s; if (scripts.TryGetValue(f.Obj, out s) && !s.Funcs.ContainsKey(f.Name)) s.Funcs[f.Name] = f; }
    }

    public static void Load(string a_data)
    {
        if (quests.Count > 0) return;
        for (int file = 0; file < SkyrimFiles.MASTERS.Length; file++)
        {
            var path = Path.Combine(a_data, SkyrimFiles.MASTERS[file]);
            if (!File.Exists(path)) { notes.Add("# not installed: " + SkyrimFiles.MASTERS[file]); continue; }
            SkyrimFiles.Quests(path, file, quests);
            SkyrimFiles.Imods(path, file, imods);
        }
        foreach (var q in quests.Values)
        {
            if (q.Edid.Length > 0 && !byEdid.ContainsKey(q.Edid)) byEdid[q.Edid] = q;
        }
        foreach (var m in imods.Values)
            if (m.Edid.Length > 0 && !imodByEdid.ContainsKey(m.Edid)) imodByEdid[m.Edid] = m;
        var shared = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var q in quests.Values)
            foreach (var name in q.Scripts.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                if (questOfScript.ContainsKey(name)) { questOfScript.Remove(name); shared.Add(name); }
                else if (!shared.Contains(name)) questOfScript[name] = q;
            }
        var compressed = new List<string>();
        foreach (var kv in SkyrimFiles.Bsa(Path.Combine(a_data, SkyrimFiles.SCRIPTS), n => n.EndsWith(".pex", StringComparison.OrdinalIgnoreCase), compressed))
            AddScript(kv.Key, kv.Value);
        foreach (var c in compressed) notes.Add("# compressed, not read: " + c);
        var loose = Path.Combine(a_data, "Scripts");
        if (Directory.Exists(loose))
            foreach (var path in Directory.GetFiles(loose, "*.pex")) AddScript(path, File.ReadAllBytes(path));
    }

    // The class a game gives a quest by its record (src/game/QuestRules.h, Classify), less the hand list of each
    // player's own quests: what a session could share.
    public static bool Typed(SkyrimFiles.Quest a_q) { return a_q.Type >= 1 && a_q.Type <= 11 && a_q.Type != 6 && a_q.Edid.Length > 0; }
    public static bool Radiant(SkyrimFiles.Quest a_q) { return a_q.Event.Length > 0 && (a_q.Flags & 0x0100) == 0; }
}

// The stages that move the player, or take their screen, controls or saving away: a stage's own fragment (Direct: MoveTo,
// SetPosition or TranslateTo of the player, a door the player is made to use, Game.FastTravel, Game.FadeOutGame, an image
// space that stays on (not a flash), a race menu, Game.DisablePlayerControls taking anything, Game.SetPlayerAIDriven,
// the player restrained, made not to move or their AI off, Game.SetInChargen), or what it calls (its quest's own script
// through kmyQuest or self, any script a property or local is typed with, a few calls deep, a branch on a literal
// argument followed one way) and the stages it sets (SetStage on its own quest or on a quest property) or the quests it
// starts (Start: their start-up stages). The player is a variable got from Game.GetPlayer, a property of the quest that
// holds form 0x14, or (where the form is not known) one named for the player. What src/game/StageRows.h's rows are
// (tools/questscan.ps1 -Write writes it). What it does not see: a scene, a package or a dialogue line a stage starts
// (their own fragments run in every game that runs the scene), a magic effect, and a reference reached through a
// variable set at run time.
public static class MoveScan
{
    static bool Is(string a, string b) { return a.Equals(b, StringComparison.OrdinalIgnoreCase); }
    static string Bare(string a_var) { var n = a_var.Replace("::", ""); return n.EndsWith("_var", StringComparison.OrdinalIgnoreCase) ? n.Substring(0, n.Length - 4) : n; }
    static bool Literal(string a) { int n; double x; return a == "True" || a == "False" || a == "None" || a.StartsWith("\"") || int.TryParse(a, out n) || double.TryParse(a, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out x); }

    static SkyrimFiles.Func Find(string a_type, string a_func, out string a_owner)
    {
        a_owner = null;
        for (int depth = 0; !string.IsNullOrEmpty(a_type) && depth < 16; depth++)
        {
            Game.Script s;
            if (!Game.scripts.TryGetValue(a_type, out s)) return null;
            SkyrimFiles.Func f;
            if (s.Funcs.TryGetValue(a_func, out f)) { a_owner = a_type; return f; }
            a_type = s.Obj.Parent;
        }
        return null;
    }

    static string TypeOf(SkyrimFiles.Func f, string a_var)
    {
        if (Is(a_var, "self")) return f.Obj;
        string t;
        if (f.Types.TryGetValue(a_var, out t)) return t;
        Game.Script s;
        for (var o = f.Obj; !string.IsNullOrEmpty(o) && Game.scripts.TryGetValue(o, out s); o = s.Obj.Parent)
            if (s.Obj.Types.TryGetValue(a_var, out t) || s.Obj.Types.TryGetValue(Bare(a_var), out t)) return t;
        return null;
    }

    // The variables of a function that hold its own quest: self (a quest's script, a fragment), kmyQuest and __temp (a
    // fragment's own), GetOwningQuest()'s result, and what is assigned or cast from one of them.
    static HashSet<string> OwnVars(SkyrimFiles.Func f)
    {
        var own = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "self", "kmyQuest", "__temp" };
        for (bool more = true; more;)
        {
            more = false;
            foreach (var i in f.Code)
            {
                string dst = null;
                if (i.Op == SkyrimFiles.CALLMETHOD && Is(i.Args[0], "GetOwningQuest") && Is(i.Args[1], "self")) dst = i.Args[2];
                else if ((i.Op == SkyrimFiles.ASSIGN || i.Op == SkyrimFiles.CAST) && own.Contains(i.Args[1])) dst = i.Args[0];
                if (dst != null && !Is(dst, "::nonevar") && own.Add(dst)) more = true;
            }
        }
        return own;
    }

    // The quest a variable of a_quest's scripts names: a property of the quest whose form is a quest, else a property or
    // variable named as a quest's editor ID (with or without a leading p).
    static SkyrimFiles.Quest QuestOf(string a_var, SkyrimFiles.Quest a_quest)
    {
        var n = Bare(a_var);
        uint form;
        SkyrimFiles.Quest q;
        if (a_quest != null && a_quest.Forms.TryGetValue(n, out form))
        {
            // the property's form as the game loads it: the quest's own file's masters come first, in MASTERS's order
            if (Game.quests.TryGetValue(form, out q)) return q;
        }
        if (Game.byEdid.TryGetValue(n, out q)) return q;
        if (n.Length > 1 && (n[0] == 'p' || n[0] == 'P') && Game.byEdid.TryGetValue(n.Substring(1), out q)) return q;
        return null;
    }

    // The instructions a call can run, in order: with the literal arguments it is called with (null: not known), a branch
    // on a parameter compared to a constant goes one way.
    static List<SkyrimFiles.Instr> Reachable(SkyrimFiles.Func f, string[] a_args)
    {
        var known = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        for (int k = 0; a_args != null && k < a_args.Length && k < f.Params.Count; k++) if (a_args[k] != null) known[f.Params[k]] = a_args[k];
        Func<string, string> value = v => Literal(v) ? v : known.ContainsKey(v) ? known[v] : null;
        var writes = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        foreach (var i in f.Code)
        {
            if (i.Op == SkyrimFiles.JMP || i.Op == SkyrimFiles.JMPT || i.Op == SkyrimFiles.JMPF || i.Op == SkyrimFiles.RETURN || i.Args.Count == 0) continue;
            var dst = i.Op == SkyrimFiles.CALLMETHOD || i.Op == SkyrimFiles.CALLSTATIC ? i.Args[2] : i.Op == SkyrimFiles.CALLPARENT ? i.Args[1] : i.Args[0];
            writes[dst] = writes.ContainsKey(dst) ? writes[dst] + 1 : 1;
        }
        foreach (var w in writes) if (w.Value > 0) known.Remove(w.Key);
        foreach (var i in f.Code)
        {
            if (i.Args.Count == 0 || (writes.ContainsKey(i.Args[0]) && writes[i.Args[0]] > 1)) continue;
            if (i.Op == SkyrimFiles.CMP_EQ) { var a = value(i.Args[1]); var b = value(i.Args[2]); if (a != null && b != null) known[i.Args[0]] = a.Equals(b, StringComparison.OrdinalIgnoreCase) ? "True" : "False"; }
            if (i.Op == SkyrimFiles.NOT) { var a = value(i.Args[1]); if (a == "True" || a == "False") known[i.Args[0]] = a == "True" ? "False" : "True"; }
            if (i.Op == SkyrimFiles.ASSIGN) { var a = value(i.Args[1]); if (a != null) known[i.Args[0]] = a; }
        }
        var seen = new bool[f.Code.Count + 1];
        var todo = new Stack<int>(); todo.Push(0);
        while (todo.Count > 0)
        {
            int pc = todo.Pop();
            if (pc < 0 || pc >= f.Code.Count || seen[pc]) continue;
            seen[pc] = true;
            var i = f.Code[pc];
            int off;
            if (i.Op == SkyrimFiles.RETURN) continue;
            if (i.Op == SkyrimFiles.JMP && int.TryParse(i.Args[0], out off)) { todo.Push(pc + off); continue; }
            if ((i.Op == SkyrimFiles.JMPT || i.Op == SkyrimFiles.JMPF) && int.TryParse(i.Args[1], out off))
            {
                var c = value(i.Args[0]);
                bool jumps = i.Op == SkyrimFiles.JMPT ? c == "True" : c == "False", falls = i.Op == SkyrimFiles.JMPT ? c == "False" : c == "True";
                if (c != "True" && c != "False") { jumps = true; falls = true; }
                if (jumps) todo.Push(pc + off);
                if (falls) todo.Push(pc + 1);
                continue;
            }
            todo.Push(pc + 1);
        }
        var code = new List<SkyrimFiles.Instr>();
        for (int k = 0; k < f.Code.Count; k++) if (seen[k]) code.Add(f.Code[k]);
        return code;
    }

    static readonly string[] GETREF = { "GetReference", "GetRef", "GetActorRef", "GetActorReference" };

    // A property or alias named for the player (PlayerRef, pPlayerRef, Alias_Player, TG05PlayerAlias), not for someone
    // of theirs (Alias_PlayerHousecarl, PlayerHorse). The fallback where the form is not known (PlayerForm).
    public static bool PlayerName(string a_var)
    {
        var n = Bare(a_var).ToLowerInvariant();
        if (n.StartsWith("alias_")) n = n.Substring(6);
        return n == "player" || n == "pplayer" || n.EndsWith("playerref") || n.EndsWith("playeralias");
    }

    // A property of a_quest's scripts that holds the player's own reference (form 0x00000014), whatever it is called
    // (ThePlayer, PlayerActor, ...): the quest's record gives each object property's form.
    public static bool PlayerForm(string a_var, SkyrimFiles.Quest a_quest)
    {
        uint form;
        return a_quest != null && a_quest.Forms.TryGetValue(Bare(a_var), out form) && form == 0x00000014;
    }

    // The image space modifier a variable holds, when known: the form a quest property of that name holds, else one whose
    // editor ID is the name (the Creation Kit fills a property in by the editor ID it is named for), else null.
    public static SkyrimFiles.Imod ImodOf(string a_var, SkyrimFiles.Quest a_quest)
    {
        uint form; SkyrimFiles.Imod m;
        if (a_quest != null && a_quest.Forms.TryGetValue(Bare(a_var), out form)) return Game.imods.TryGetValue(form, out m) ? m : null;
        return Game.imodByEdid.TryGetValue(Bare(a_var), out m) ? m : null;
    }

    // Whether an image space put on the screen with Apply or PopTo goes off by itself soon: one that plays a few seconds
    // and ends (DNAM: animatable, a duration up to BRIEF seconds; a glow, a blur, a flash). One that is not known (a
    // variable set at run time, a property of a script on a reference) counts as staying: a row that fires too often
    // costs a stage made only with its friend near, a missed one a friend left behind a held screen.
    public const float BRIEF = 10;
    public static bool Brief(string a_imod, SkyrimFiles.Quest a_quest)
    {
        var m = ImodOf(a_imod, a_quest);
        return m != null && m.Animatable && m.Duration > 0 && m.Duration <= BRIEF;
    }

    // Whether a call's argument a_at (counted from its first parameter) is anything but the literal False: True, or a value
    // known only at run time (a variable, a parameter), which may be True. Left out (a default): a_default.
    static bool Takes(SkyrimFiles.Instr a_i, int a_first, int a_at, bool a_default)
    {
        int k = a_first + a_at;
        if (k >= a_i.Args.Count) return a_default;
        return !Is(a_i.Args[k], "False") && !Is(a_i.Args[k], "0");
    }

    static readonly string[] MOVES = { "MoveTo", "MoveToNode", "MoveToMyEditorLocation", "MoveToInteractionLocation", "MoveToPackageLocation", "SetPosition",
        "TranslateTo", "TranslateToRef", "SplineTranslateTo", "SplineTranslateToRef", "SplineTranslateToRefNode" };

    // What a function's own code does to the player that is looked for (not through what it calls), or null: it moves
    // them (MoveTo and its kin, SetPosition, TranslateTo, a door they are made to use, Game.FastTravel), takes their
    // screen and leaves it so (Game.FadeOutGame, an image space that stays on: Brief; a race menu), their controls
    // (Game.DisablePlayerControls taking anything, Game.SetPlayerAIDriven), freezes them (SetRestrained, SetDontMove,
    // EnableAI(False)) or stops saving and waiting (Game.SetInChargen). An argument known only at run time counts as
    // taking: a row that fires too often costs a stage made only with its friend near, a missed one a frozen friend.
    // A fade or an image space the same code takes off again further on, with no branch or loop between (a wait is
    // fine: the Greybeards' word fade, white for two seconds), is a flash, not a hold. One taken off only past a branch,
    // in a loop waiting on the player (Molag Bal's haunting) or by another stage stays a hold.
    // a_f: the function (the types of its variables); a_quest: the quest whose script or fragment it is, when known.
    public static string Direct(List<SkyrimFiles.Instr> a_code, SkyrimFiles.Func a_f, SkyrimFiles.Quest a_quest)
    {
        var players = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Func<string, bool> player = v => players.Contains(v) || PlayerName(v) || PlayerForm(v, a_quest);
        // the first of each kind, said by the worst: a move, then the player frozen, controls, chargen, the screen
        var found = new string[5];
        Action<int, string> note = (k, w) => { if (found[k] == null) found[k] = w; };
        // what is on the screen and not yet taken off in straight code: (an image space's variable, "|crossfade|" or
        // "|fade|"; what it is), held for good at the next branch or the end
        var pending = new List<KeyValuePair<string, string>>();
        Action<string> off = k => pending.RemoveAll(p => Is(p.Key, k));
        Action<string, string> put = (k, w) => pending.Add(new KeyValuePair<string, string>(k, w));
        Action held = () => { foreach (var p in pending) note(4, p.Value); pending.Clear(); };
        foreach (var i in a_code)
        {
            if (i.Op == SkyrimFiles.JMP || i.Op == SkyrimFiles.JMPT || i.Op == SkyrimFiles.JMPF) { held(); continue; }
            // only one cross-fade is on at a time: RemoveCrossFade takes it off
            if (i.Op == SkyrimFiles.CALLSTATIC && Is(i.Args[0], "ImageSpaceModifier") && Is(i.Args[1], "RemoveCrossFade")) { off("|crossfade|"); continue; }
            if (i.Op == SkyrimFiles.CALLSTATIC && Is(i.Args[0], "game"))
            {
                var m = i.Args[1];
                if (Is(m, "GetPlayer")) { players.Add(i.Args[2]); continue; }
                if (Is(m, "FastTravel")) note(0, "Game.FastTravel " + Bare(i.Args.Count > 3 ? i.Args[3] : ""));
                // abMovement, abFighting, abCamSwitch, abLooking, abSneaking, abMenu, abActivate, abJournalTabs: any one
                if (Is(m, "DisablePlayerControls") && (i.Args.Count <= 3 || Enumerable.Range(0, 8).Any(k => Takes(i, 3, k, false)))) note(2, "Game.DisablePlayerControls");
                if (Is(m, "SetPlayerAIDriven") && Takes(i, 3, 0, true)) note(2, "Game.SetPlayerAIDriven");
                if (Is(m, "SetInChargen") && (Takes(i, 3, 0, false) || Takes(i, 3, 1, false))) note(3, "Game.SetInChargen");
                // FadeOutGame(abFadingOut, abBlackFade, afSecsBeforeFade, afFadeDuration): fading out stays black (or white)
                // until faded in; fading in holds the screen as it is for afSecsBeforeFade first (FadeOutGame(False, True,
                // 50, 1) holds black for 50 s), and a short one ends any fade.
                if (Is(m, "FadeOutGame"))
                {
                    if (Takes(i, 3, 0, false)) { put("|fade|", "Game.FadeOutGame"); continue; }
                    off("|fade|");
                    float secs;
                    if (!(i.Args.Count > 5 && float.TryParse(i.Args[5], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out secs) && secs <= BRIEF))
                        put("|fade|", "Game.FadeOutGame holding " + Bare(i.Args.Count > 5 ? i.Args[5] : "?") + " s");
                }
                if (Is(m, "ShowRaceMenu") || Is(m, "ShowLimitedRaceMenu")) note(4, "Game." + m);
                continue;
            }
            if (i.Op == SkyrimFiles.ASSIGN || i.Op == SkyrimFiles.CAST) { if (player(i.Args[1])) players.Add(i.Args[0]); else players.Remove(i.Args[0]); continue; }
            if (i.Op != SkyrimFiles.CALLMETHOD) continue;
            var name = i.Args[0]; var self = i.Args[1];
            if (GETREF.Any(g => Is(name, g)) && (PlayerName(self) || PlayerForm(self, a_quest))) { players.Add(i.Args[2]); continue; }
            if (MOVES.Any(n => Is(name, n)) && player(self))
                note(0, (Is(name, "SetPosition") || name.IndexOf("Translate", StringComparison.OrdinalIgnoreCase) >= 0 ? name : "MoveTo") + " " + Bare(i.Args.Count > 3 ? i.Args[3] : ""));
            if (Is(name, "Activate") && i.Args.Count > 3 && player(i.Args[3]) && Bare(self).IndexOf("Door", StringComparison.OrdinalIgnoreCase) >= 0) note(0, "the player through " + Bare(self));
            if ((Is(name, "SetRestrained") || Is(name, "SetDontMove")) && player(self) && Takes(i, 3, 0, true)) note(1, name + " on the player");
            if (Is(name, "EnableAI") && player(self) && i.Args.Count > 3 && !Is(i.Args[3], "True") && !Is(i.Args[3], "1")) note(1, "EnableAI(False) on the player");
            if (a_f != null && Is(TypeOf(a_f, self) ?? "", "ImageSpaceModifier"))
            {
                // Apply: on until Remove (or played out if brief); PopTo: this one off, the target on; ApplyCrossFade: in
                // place of the cross-fade before, on until RemoveCrossFade
                var to = Is(name, "PopTo") && i.Args.Count > 3 ? i.Args[3] : self;
                if (Is(name, "Remove") || Is(name, "PopTo")) off(Bare(self));
                if ((Is(name, "Apply") || Is(name, "PopTo")) && !Brief(to, a_quest)) put(Bare(to), "ImageSpaceModifier." + name + " " + Bare(to));
                if (Is(name, "ApplyCrossFade")) { off("|crossfade|"); put("|crossfade|", "ImageSpaceModifier." + name + " " + Bare(self)); }
            }
            players.Remove(i.Args[2]);
        }
        held();
        return found.FirstOrDefault(w => w != null);
    }

    // What a row's chain ends in, as src/game/StageRule.h's StageEffect.
    public static string Effect(string a_why)
    {
        var at = a_why.LastIndexOf(": ", StringComparison.Ordinal);
        var w = at >= 0 ? a_why.Substring(at + 2) : a_why;
        if (w.StartsWith("Game.FadeOutGame") || w.StartsWith("ImageSpaceModifier.") || w.StartsWith("Game.Show")) return "kScreen";
        if (w.StartsWith("Game.DisablePlayerControls") || w.StartsWith("Game.SetPlayerAIDriven")) return "kControls";
        if (w.StartsWith("SetRestrained") || w.StartsWith("SetDontMove") || w.StartsWith("EnableAI")) return "kFrozen";
        if (w.StartsWith("Game.SetInChargen")) return "kChargen";
        return "kMove";
    }

    // A walk through what a function or a stage runs (the sister project's MoveScan.Walk, for Skyrim's scripts).
    public class Walk
    {
        public Func<List<SkyrimFiles.Instr>, SkyrimFiles.Func, SkyrimFiles.Quest, string> Direct;
        readonly Dictionary<string, string> funcs = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, string> stages = new Dictionary<string, string>();

        // What a function does that is looked for, itself or through what it calls and sets, or null: the chain of what it
        // calls and sets, ending in Direct's answer. a_quest: the quest whose script or fragment it is, when known.
        public string Function(SkyrimFiles.Func f, string[] a_args, SkyrimFiles.Quest a_quest, HashSet<string> a_seen)
        {
            var key = f.Obj + "." + f.State + "." + f.Name + "(" + (a_args == null ? "" : string.Join(",", a_args.Select(a => a ?? "?").ToArray())) + ")@" + (a_quest == null ? "" : a_quest.Id.ToString("X8"));
            string cached;
            if (funcs.TryGetValue(key, out cached)) return cached;
            if (!a_seen.Add(key)) return null;
            var code = Reachable(f, a_args);
            var own = OwnVars(f);
            string why = Direct(code, f, a_quest);
            if (why != null) why = f.Obj + "." + f.Name + ": " + why;
            for (int n = 0; why == null && n < code.Count; n++)
            {
                var i = code[n];
                if (i.Op != SkyrimFiles.CALLMETHOD && i.Op != SkyrimFiles.CALLPARENT && i.Op != SkyrimFiles.CALLSTATIC) continue;
                if (i.Op == SkyrimFiles.CALLMETHOD && (Is(i.Args[0], "SetStage") || Is(i.Args[0], "SetCurrentStageID") || Is(i.Args[0], "Start")))
                {
                    bool mine = own.Contains(i.Args[1]);
                    var target = mine ? a_quest : QuestOf(i.Args[1], a_quest);
                    if (target == null) continue;
                    mine = mine || target == a_quest;
                    var list = new List<int>();
                    int s;
                    // a stage set is the stage's own fragment; a quest started runs its start-up stages (a stage set on a
                    // quest that is not running starts it too, but a script sets a stage of a quest it knows is running:
                    // MQ102 10 ends Helgen by MQ101 900, which never runs MQ101's start again)
                    if (!Is(i.Args[0], "Start") && i.Args.Count > 3 && int.TryParse(i.Args[3], out s)) list.Add(s);
                    if (!mine && Is(i.Args[0], "Start")) list.AddRange(target.Startup());
                    foreach (var st in list)
                    {
                        var w = Stage(target, st, a_seen);
                        if (w != null) { why = (mine ? "" : target.Edid + " ") + st + " -> " + w; break; }
                    }
                    continue;
                }
                string type, name;
                if (i.Op == SkyrimFiles.CALLSTATIC) { type = i.Args[0]; name = i.Args[1]; }
                else if (i.Op == SkyrimFiles.CALLPARENT) { Game.Script sc; type = Game.scripts.TryGetValue(f.Obj, out sc) ? sc.Obj.Parent : null; name = i.Args[0]; }
                else { type = TypeOf(f, i.Args[1]); name = i.Args[0]; }
                if (type == null) continue;
                string owner;
                var g = Find(type, name, out owner);
                if (g == null) continue;
                // a call on the quest's own script keeps the quest; one through a variable typed with the script of exactly
                // one quest is that quest's; any other script's is no quest's
                SkyrimFiles.Quest q = null;
                if (i.Op == SkyrimFiles.CALLPARENT || (i.Op == SkyrimFiles.CALLMETHOD && own.Contains(i.Args[1]))) q = a_quest;
                else if (i.Op == SkyrimFiles.CALLMETHOD) Game.questOfScript.TryGetValue(type, out q);
                int first = i.Op == SkyrimFiles.CALLPARENT ? 2 : 3;
                var args = i.Args.Skip(first).Select(a => Literal(a) ? a : null).ToArray();
                var w2 = Function(g, args, q, a_seen);
                if (w2 != null) why = owner + "." + name + " -> " + w2;
            }
            a_seen.Remove(key);
            // only finds are kept: a none found while a caller up the chain was still open may have missed a cycle through it
            if (why != null) funcs[key] = why;
            return why;
        }

        public string Stage(SkyrimFiles.Quest a_quest, int a_stage, HashSet<string> a_seen)
        {
            var key = a_quest.Id.ToString("X8") + ":" + a_stage;
            string cached;
            if (stages.TryGetValue(key, out cached)) return cached;
            if (!a_seen.Add(key)) return null;
            string why = null;
            List<string> names;
            Game.Script script;
            if (a_quest.Fragments.TryGetValue(a_stage, out names) && Game.scripts.TryGetValue(a_quest.FragmentScript, out script))
                foreach (var name in names)
                {
                    SkyrimFiles.Func f;
                    if (!script.Funcs.TryGetValue(name, out f)) continue;
                    why = Function(f, null, a_quest, a_seen);
                    if (why != null) break;
                }
            a_seen.Remove(key);
            if (why != null) stages[key] = why;
            return why;
        }

        public string Stage(SkyrimFiles.Quest a_quest, int a_stage) { return Stage(a_quest, a_stage, new HashSet<string>()); }
    }

    public static readonly Walk moves = new Walk { Direct = Direct };

    public class Row { public SkyrimFiles.Quest Quest; public int Stage; public string Why; }

    // Every stage of a quest a session could share (Game.Typed) that moves the player, sorted by editor ID (any case)
    // and stage: the order src/game/StageRows.h keeps.
    public static List<Row> Rows(string a_data)
    {
        Game.Load(a_data);
        var rows = new List<Row>();
        foreach (var q in Game.quests.Values)
        {
            if (!Game.Typed(q)) continue;
            foreach (var st in q.Stages.Select(s => s.Index).Concat(q.Fragments.Keys).Distinct())
            {
                var why = moves.Stage(q, st);
                if (why != null) rows.Add(new Row { Quest = q, Stage = st, Why = why });
            }
        }
        rows.Sort((a, b) =>
        {
            int c = string.Compare(a.Quest.Edid, b.Quest.Edid, StringComparison.OrdinalIgnoreCase);
            return c != 0 ? c : a.Stage.CompareTo(b.Stage);
        });
        return rows;
    }

    public static string Run(string a_data)
    {
        var sb = new StringBuilder();
        foreach (var r in Rows(a_data))
            sb.AppendLine(string.Format("{0:X8} {1,-34} {2,5}  {3}", r.Quest.Id, r.Quest.Edid, r.Stage, r.Why));
        foreach (var n in Game.notes) sb.AppendLine(n);
        sb.AppendLine(string.Format("# quests={0} typed={1} scripts={2}", Game.quests.Count, Game.quests.Values.Count(Game.Typed), Game.scripts.Count));
        return sb.ToString();
    }

    // An image space modifier as the scan sees it, for -Dump.
    public static string Describe(SkyrimFiles.Imod a_m)
    {
        return string.Format("(image space {0}: {1})", a_m.Edid, a_m.Animatable && a_m.Duration > 0 ? "plays " + a_m.Duration.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture) + " s and ends" : "stays on until taken off");
    }

    // One quest's stage fragments, disassembled (each stage's functions), with what the quest's record says and what each
    // image space property does: for checking a row by hand. An image space's editor ID gives what it does alone.
    public static string Dump(string a_data, string a_edid)
    {
        Game.Load(a_data);
        SkyrimFiles.Quest q;
        SkyrimFiles.Imod im;
        if (!Game.byEdid.TryGetValue(a_edid, out q)) return Game.imodByEdid.TryGetValue(a_edid, out im) ? string.Format("# {0:X8} {1}", im.Id, Describe(im)) : "# no quest or image space " + a_edid;
        var sb = new StringBuilder();
        sb.AppendLine(string.Format("# {0} {1:X8} type={2} flags={3:X4} event={4} file={5} fragments={6} scripts={7}", q.Edid, q.Id, q.Type, q.Flags, q.Event, SkyrimFiles.MASTERS[q.File], q.FragmentScript, string.Join(",", q.Scripts.ToArray())));
        foreach (var st in q.Stages)
            sb.AppendLine(string.Format("# stage {0} flags={1:X2}{2}{3}", st.Index, st.Flags, st.Completes ? " completes" : "", st.Fails ? " fails" : ""));
        foreach (var kv in q.Forms)
        {
            SkyrimFiles.Imod m;
            sb.AppendLine(string.Format("# property {0} = {1:X8}{2}", kv.Key, kv.Value, Game.imods.TryGetValue(kv.Value, out m) ? " " + Describe(m) : ""));
        }
        Game.Script script;
        if (!Game.scripts.TryGetValue(q.FragmentScript, out script)) return sb.ToString();
        foreach (var kv in q.Fragments.OrderBy(k => k.Key))
            foreach (var name in kv.Value)
            {
                SkyrimFiles.Func f;
                if (!script.Funcs.TryGetValue(name, out f)) continue;
                sb.AppendLine("stage " + kv.Key + ": " + name);
                foreach (var i in f.Code) sb.AppendLine("    " + SkyrimFiles.Text(i));
            }
        return sb.ToString();
    }

    // src/game/StageRows.h: the rows, each with what was found (ASCII, CRLF).
    public static string Header(string a_data)
    {
        var rows = Rows(a_data);
        var sb = new StringBuilder();
        sb.Append("#pragma once\r\n\r\n#include \"StageRule.h\"\r\n\r\n#include <array>\r\n\r\n");
        sb.Append("// Written by tools/questscan.ps1 -Write from the game's own files (Skyrim.esm, Update.esm, Dawnguard.esm,\r\n");
        sb.Append("// HearthFires.esm, Dragonborn.esm and the scripts in Skyrim - Misc.bsa); do not edit by hand, run it again. Each row is a\r\n");
        sb.Append("// stage of a quest a session may share whose fragment moves the player, or takes their screen, controls or saving away,\r\n");
        sb.Append("// itself or through what it calls, sets or starts (tools/questscan.cs, MoveScan): made only with the friend who did it\r\n");
        sb.Append("// near (StageEffect: what it does, for the log).\r\n");
        sb.Append("// The comment after each row is the chain the scan followed to it. A row ending in true is its quest's start-up stage:\r\n");
        sb.Append("// any stage made while the quest is not started starts it and runs that stage too (QuestRules::StageRowHere). Sorted by\r\n");
        sb.Append("// editor ID (any case), then stage.\r\n");
        sb.Append("namespace QuestRules\r\n{\r\n");
        sb.Append("\tinline constexpr std::array<StageRule, " + rows.Count + "> STAGE_RULES{ {\r\n");
        foreach (var r in rows)
        {
            var why = r.Why.Replace("\r", " ").Replace("\n", " ");
            var startup = r.Quest.Startup().Contains(r.Stage) ? ", true" : "";
            sb.Append(string.Format("\t\t{{ \"{0}\", {1}, StageVerdict::kMoves, StageEffect::{2}{3} }},  // {4}\r\n", r.Quest.Edid, r.Stage, Effect(r.Why), startup, why));
        }
        sb.Append("\t} };\r\n}\r\n");
        return sb.ToString();
    }
}
