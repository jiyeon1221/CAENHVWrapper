using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;

static class HV
{
    // Linux: use libCAENHVWrapper.so
    [DllImport("libCAENHVWrapper.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern int CAENHV_InitSystem(
        int systemType, int linkType, string address,
        string username, string password, out int handle);

    [DllImport("libCAENHVWrapper.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern int CAENHV_DeinitSystem(int handle);

    [DllImport("libCAENHVWrapper.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern int CAENHV_SetChParam(
        int handle, ushort slot, string parName,
        ushort chNum, ushort[] chList, IntPtr parValues);

    [DllImport("libCAENHVWrapper.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern int CAENHV_GetChParam(
        int handle, ushort slot, string parName,
        ushort chNum, ushort[] chList, IntPtr parValues);

    [DllImport("libCAENHVWrapper.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr CAENHV_GetError(int handle);
}

class Program
{
    const string FIXED_IP   = "192.168.100.2";
    const string FIXED_USER = "admin";
    const string FIXED_PASS = "admin";
    const int SYSTEM_TYPE = 2; // SY4527
    const int LINK_TYPE   = 0; // TCP/IP

    // param types: f = float, u = ushort
    static readonly Dictionary<string,char> ParType = new(StringComparer.OrdinalIgnoreCase) {
        ["VMon"]  = 'f',
        ["IMon"]  = 'f',
        ["V0Set"] = 'f',
        ["I0Set"] = 'f',
        ["RUp"]   = 'f',
        ["RDWn"]  = 'f',
        ["SVMax"] = 'f',
        ["Pw"]    = 'u',
        ["PDwn"]  = 'u',
        ["Status"]= 'u'
    };

    static int Err(string msg, int code = 1) { Console.Error.WriteLine(msg); return code; }

    static int WithHV(Func<int,int> body)
    {
        int h;
        int rc = HV.CAENHV_InitSystem(SYSTEM_TYPE, LINK_TYPE, FIXED_IP, FIXED_USER, FIXED_PASS, out h);
        if (rc != 0)
        {
            Console.Error.WriteLine($"InitSystem failed rc={rc} (sys={SYSTEM_TYPE}, link={LINK_TYPE}, ip={FIXED_IP})");
            return 1;
        }
        try { return body(h); }
        finally { HV.CAENHV_DeinitSystem(h); }
    }

    static ushort[] ParseChannels(string s)
    {
        return s.Split(',', StringSplitOptions.RemoveEmptyEntries)
                .SelectMany(tok =>
                {
                    var t = tok.Trim();
                    if (t.Contains('-'))
                    {
                        var sp = t.Split('-');
                        int a = int.Parse(sp[0]);
                        int b = int.Parse(sp[1]);
                        return Enumerable.Range(Math.Min(a,b), Math.Abs(b-a)+1);
                    }
                    return new[] { int.Parse(t) };
                })
                .Select(i => (ushort)i)
                .ToArray();
    }

    static IntPtr FloatArray(float[] v)
    {
        IntPtr p = Marshal.AllocHGlobal(sizeof(float) * v.Length);
        var bytes = new byte[v.Length * sizeof(float)];
        Buffer.BlockCopy(v, 0, bytes, 0, bytes.Length);
        Marshal.Copy(bytes, 0, p, bytes.Length);
        return p;
    }

    static float[] ReadFloatArray(IntPtr p, int n)
    {
        var bytes = new byte[n * sizeof(float)];
        Marshal.Copy(p, bytes, 0, bytes.Length);
        var res = new float[n];
        Buffer.BlockCopy(bytes, 0, res, 0, bytes.Length);
        return res;
    }

    static IntPtr UShortArray(ushort[] v)
    {
        IntPtr p = Marshal.AllocHGlobal(sizeof(ushort) * v.Length);
        var bytes = new byte[v.Length * sizeof(ushort)];
        Buffer.BlockCopy(v, 0, bytes, 0, bytes.Length);
        Marshal.Copy(bytes, 0, p, bytes.Length);
        return p;
    }

    static ushort[] ReadUShortArray(IntPtr p, int n)
    {
        var bytes = new byte[n * sizeof(ushort)];
        Marshal.Copy(p, bytes, 0, bytes.Length);
        var res = new ushort[n];
        Buffer.BlockCopy(bytes, 0, res, 0, bytes.Length);
        return res;
    }

    static string HvErr(int h, int rc)
    {
        try
        {
            var ptr = HV.CAENHV_GetError(h);
            return $"rc={rc} (0x{rc:X}) : {Marshal.PtrToStringAnsi(ptr)}";
        }
        catch
        {
            return $"rc={rc} (0x{rc:X})";
        }
    }

    static string Get(string[] args, string k, string def = null)
    {
        int i = Array.IndexOf(args, $"--{k}");
        return (i >= 0 && i + 1 < args.Length) ? args[i + 1] : def;
    }

    // Commands
    static int CmdCheck()
    {
        int h;
        int rc = HV.CAENHV_InitSystem(SYSTEM_TYPE, LINK_TYPE, FIXED_IP, FIXED_USER, FIXED_PASS, out h);
        if (rc != 0)
        {
            Console.WriteLine($"CHECK FAIL: rc={rc}");
            return 1;
        }
        Console.WriteLine("CHECK OK");
        HV.CAENHV_DeinitSystem(h);
        return 0;
    }

    static int CmdRead(ushort slot, string channels)
    {
        return WithHV(h =>
        {
            var chs = ParseChannels(channels);
            int n = chs.Length;

            IntPtr vmon = Marshal.AllocHGlobal(sizeof(float) * n);
            IntPtr imon = Marshal.AllocHGlobal(sizeof(float) * n);
            IntPtr pw   = Marshal.AllocHGlobal(sizeof(ushort) * n);

            int rc1 = HV.CAENHV_GetChParam(h, slot, "VMon", (ushort)n, chs, vmon);
            int rc2 = HV.CAENHV_GetChParam(h, slot, "IMon", (ushort)n, chs, imon);
            int rc3 = HV.CAENHV_GetChParam(h, slot, "Pw",   (ushort)n, chs, pw);

            if (rc1 != 0 || rc2 != 0 || rc3 != 0)
                return Err($"read failed: {HvErr(h, rc1)} / {HvErr(h, rc2)} / {HvErr(h, rc3)}");

            var V = ReadFloatArray(vmon, n);
            var I = ReadFloatArray(imon, n);
            var P = ReadUShortArray(pw, n);

            Console.WriteLine("slot,ch,pw,vmon,imon");
            for (int i = 0; i < n; i++)
                Console.WriteLine($"{slot},{chs[i]},{P[i]},{V[i]},{I[i]}");

            Marshal.FreeHGlobal(vmon);
            Marshal.FreeHGlobal(imon);
            Marshal.FreeHGlobal(pw);
            return 0;
        });
    }

    static int CmdOnOff(ushort slot, string channels, bool on)
    {
        return WithHV(h =>
        {
            var chs = ParseChannels(channels);
            IntPtr buf = UShortArray(Enumerable.Repeat((ushort)(on ? 1 : 0), chs.Length).ToArray());
            int rc = HV.CAENHV_SetChParam(h, slot, "Pw", (ushort)chs.Length, chs, buf);
            Marshal.FreeHGlobal(buf);
            if (rc != 0) return Err($"set Pw failed {HvErr(h, rc)}");
            Console.WriteLine($"Power={(on ? 1 : 0)} OK");
            return 0;
        });
    }

    static int CmdSetV(ushort slot, string channels, float v)
    {
        return WithHV(h =>
        {
            var chs = ParseChannels(channels);
            IntPtr buf = FloatArray(Enumerable.Repeat(v, chs.Length).ToArray());
            int rc = HV.CAENHV_SetChParam(h, slot, "V0Set", (ushort)chs.Length, chs, buf);
            Marshal.FreeHGlobal(buf);
            if (rc != 0) return Err($"set V0Set failed {HvErr(h, rc)}");
            Console.WriteLine($"V0Set={v} OK");
            return 0;
        });
    }

    static int CmdSetI(ushort slot, string channels, float i)
    {
        return WithHV(h =>
        {
            var chs = ParseChannels(channels);
            IntPtr buf = FloatArray(Enumerable.Repeat(i, chs.Length).ToArray());
            int rc = HV.CAENHV_SetChParam(h, slot, "I0Set", (ushort)chs.Length, chs, buf);
            Marshal.FreeHGlobal(buf);
            if (rc != 0) return Err($"set I0Set failed {HvErr(h, rc)}");
            Console.WriteLine($"I0Set={i} OK");
            return 0;
        });
    }

    static int CmdSetRamp(ushort slot, string channels, float rup, float rdn)
    {
        return WithHV(h =>
        {
            var chs = ParseChannels(channels);
            IntPtr up = FloatArray(Enumerable.Repeat(rup, chs.Length).ToArray());
            IntPtr dn = FloatArray(Enumerable.Repeat(rdn, chs.Length).ToArray());
            int rc1 = HV.CAENHV_SetChParam(h, slot, "RUp",  (ushort)chs.Length, chs, up);
            int rc2 = HV.CAENHV_SetChParam(h, slot, "RDWn", (ushort)chs.Length, chs, dn);
            Marshal.FreeHGlobal(up);
            Marshal.FreeHGlobal(dn);
            if (rc1 != 0 || rc2 != 0) return Err($"set ramp failed: {HvErr(h, rc1)} / {HvErr(h, rc2)}");
            Console.WriteLine("Ramp OK");
            return 0;
        });
    }

    static int CmdRampTo(ushort slot, string channels, float vTarget, float tol, int timeoutMs, int pollMs)
    {
        return WithHV(h =>
        {
            var chs = ParseChannels(channels);
            int n = chs.Length;

            IntPtr buf = FloatArray(Enumerable.Repeat(vTarget, n).ToArray());
            int rc = HV.CAENHV_SetChParam(h, slot, "V0Set", (ushort)n, chs, buf);
            Marshal.FreeHGlobal(buf);
            if (rc != 0) return Err($"set V0Set failed {HvErr(h, rc)}");

            IntPtr onBuf = UShortArray(Enumerable.Repeat((ushort)1, n).ToArray());
            rc = HV.CAENHV_SetChParam(h, slot, "Pw", (ushort)n, chs, onBuf);
            Marshal.FreeHGlobal(onBuf);
            if (rc != 0) return Err($"set Pw=1 failed {HvErr(h, rc)}");

            var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            IntPtr vmon = Marshal.AllocHGlobal(sizeof(float) * n);

            while (true)
            {
                rc = HV.CAENHV_GetChParam(h, slot, "VMon", (ushort)n, chs, vmon);
                if (rc != 0)
                {
                    Marshal.FreeHGlobal(vmon);
                    return Err($"read VMon failed {HvErr(h, rc)}");
                }

                var V = ReadFloatArray(vmon, n);
                bool allIn = V.All(x => Math.Abs(x - vTarget) <= tol);
                Console.WriteLine($"{DateTime.Now:HH:mm:ss} VMon=[{string.Join(", ", V.Select(v=>v.ToString("F2")))}] target={vTarget} tol=±{tol}");
                if (allIn)
                {
                    Marshal.FreeHGlobal(vmon);
                    Console.WriteLine("OK ramp-to reached.");
                    return 0;
                }

                if (DateTime.UtcNow > deadline)
                {
                    Marshal.FreeHGlobal(vmon);
                    return Err("Timeout while ramping");
                }
                Thread.Sleep(pollMs);
            }
        });
    }

    static int CmdRampDownAndOff(ushort slot, string channels, float tol, int timeoutMs, int pollMs)
    {
        return WithHV(h =>
        {
            var chs = ParseChannels(channels);
            int n = chs.Length;

            IntPtr buf = FloatArray(Enumerable.Repeat(0f, n).ToArray());
            int rc = HV.CAENHV_SetChParam(h, slot, "V0Set", (ushort)n, chs, buf);
            Marshal.FreeHGlobal(buf);
            if (rc != 0) return Err($"set V0Set(0) failed {HvErr(h, rc)}");

            var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            IntPtr vmon = Marshal.AllocHGlobal(sizeof(float) * n);

            while (true)
            {
                rc = HV.CAENHV_GetChParam(h, slot, "VMon", (ushort)n, chs, vmon);
                if (rc != 0)
                {
                    Marshal.FreeHGlobal(vmon);
                    return Err($"read VMon failed {HvErr(h, rc)}");
                }
                var V = ReadFloatArray(vmon, n);
                bool allLow = V.All(x => Math.Abs(x) <= tol);
                Console.WriteLine($"{DateTime.Now:HH:mm:ss} VMon=[{string.Join(", ", V.Select(v=>v.ToString("F2")))}] target=0 tol=±{tol}");
                if (allLow) break;
                if (DateTime.UtcNow > deadline)
                {
                    Marshal.FreeHGlobal(vmon);
                    return Err("Timeout while ramping down");
                }
                Thread.Sleep(pollMs);
            }
            Marshal.FreeHGlobal(vmon);

            IntPtr offBuf = UShortArray(Enumerable.Repeat((ushort)0, n).ToArray());
            rc = HV.CAENHV_SetChParam(h, slot, "Pw", (ushort)n, chs, offBuf);
            Marshal.FreeHGlobal(offBuf);
            if (rc != 0) return Err($"set Pw=0 failed {HvErr(h, rc)}");

            Console.WriteLine("OK ramp-down & OFF.");
            return 0;
        });
    }

    static int CmdParamGet(ushort slot, string channels, string parName)
    {
        if (string.IsNullOrWhiteSpace(parName)) return Err("Missing --param");
        if (!ParType.TryGetValue(parName, out var kind)) return Err($"Unknown param: {parName}");

        return WithHV(h =>
        {
            var chs = ParseChannels(channels);
            int n = chs.Length;

            if (kind == 'f')
            {
                IntPtr buf = Marshal.AllocHGlobal(sizeof(float) * n);
                int rc = HV.CAENHV_GetChParam(h, slot, parName, (ushort)n, chs, buf);
                if (rc != 0)
                {
                    Marshal.FreeHGlobal(buf);
                    return Err($"get {parName} failed {HvErr(h, rc)}");
                }
                var vals = ReadFloatArray(buf, n);
                Marshal.FreeHGlobal(buf);
                Console.WriteLine($"{parName}=[{string.Join(",", vals.Select(v=>v.ToString("G")))}]");
            }
            else
            {
                IntPtr buf = Marshal.AllocHGlobal(sizeof(ushort) * n);
                int rc = HV.CAENHV_GetChParam(h, slot, parName, (ushort)n, chs, buf);
                if (rc != 0)
                {
                    Marshal.FreeHGlobal(buf);
                    return Err($"get {parName} failed {HvErr(h, rc)}");
                }
                var vals = ReadUShortArray(buf, n);
                Marshal.FreeHGlobal(buf);
                Console.WriteLine($"{parName}=[{string.Join(",", vals)}]");
            }
            return 0;
        });
    }

    static int CmdParamSet(ushort slot, string channels, string parName, string value)
    {
        if (string.IsNullOrWhiteSpace(parName) || string.IsNullOrWhiteSpace(value)) return Err("Missing --param or --value");
        if (!ParType.TryGetValue(parName, out var kind)) return Err($"Unknown param: {parName}");

        return WithHV(h =>
        {
            var chs = ParseChannels(channels);
            int n = chs.Length;

            if (kind == 'f')
            {
                if (!float.TryParse(value, out var fv)) return Err($"Invalid float: {value}");
                IntPtr buf = FloatArray(Enumerable.Repeat(fv, n).ToArray());
                int rc = HV.CAENHV_SetChParam(h, slot, parName, (ushort)n, chs, buf);
                Marshal.FreeHGlobal(buf);
                if (rc != 0) return Err($"set {parName} failed {HvErr(h, rc)}");
                Console.WriteLine($"{parName}={fv} OK");
            }
            else
            {
                if (!ushort.TryParse(value, out var uv)) return Err($"Invalid ushort: {value}");
                IntPtr buf = UShortArray(Enumerable.Repeat(uv, n).ToArray());
                int rc = HV.CAENHV_SetChParam(h, slot, parName, (ushort)n, chs, buf);
                Marshal.FreeHGlobal(buf);
                if (rc != 0) return Err($"set {parName} failed {HvErr(h, rc)}");
                Console.WriteLine($"{parName}={uv} OK");
            }
            return 0;
        });
    }

    static int Main(string[] args)
    {
        if (args.Length == 0 || args[0] == "--help" || args[0] == "-h")
        {
            Console.WriteLine(
@"hvctl (Linux build, SY4527 @ 192.168.100.2 / admin)

USAGE:
  hvctl check
  hvctl read      --slot S --channels CH
  hvctl on        --slot S --channels CH
  hvctl off       --slot S --channels CH
  hvctl set-v     --slot S --channels CH --voltage V
  hvctl set-i     --slot S --channels CH --imax I
  hvctl set-ramp  --slot S --channels CH --rup R --rdn D
  hvctl ramp-to   --slot S --channels CH --voltage V [--tol 5] [--timeout-ms 600000] [--poll-ms 500]
  hvctl ramp-down --slot S --channels CH [--tol 5] [--timeout-ms 600000] [--poll-ms 500]
  hvctl param-get --slot S --channels CH --param Name
  hvctl param-set --slot S --channels CH --param Name --value Val

PARAM TYPES:
  float : VMon, IMon, V0Set, I0Set, RUp, RDWn, SVMax
  ushort: Pw, PDwn, Status

NOTE:
  channels supports comma/range: 0,1,2,4-7");
            return 0;
        }

        string cmd = args[0];

        if (cmd == "check") return CmdCheck();

        string slotStr = Get(args, "slot");
        string chStr   = Get(args, "channels");
        if (!ushort.TryParse(slotStr, out var slot)) return Err("Missing/invalid --slot");
        if (string.IsNullOrWhiteSpace(chStr))        return Err("Missing --channels");

        switch (cmd)
        {
            case "read":      return CmdRead(slot, chStr);
            case "on":        return CmdOnOff(slot, chStr, true);
            case "off":       return CmdOnOff(slot, chStr, false);
            case "set-v":
                if (!float.TryParse(Get(args, "voltage"), out var v0)) return Err("Missing/invalid --voltage");
                return CmdSetV(slot, chStr, v0);
            case "set-i":
                if (!float.TryParse(Get(args, "imax"), out var i0)) return Err("Missing/invalid --imax");
                return CmdSetI(slot, chStr, i0);
            case "set-ramp":
                if (!float.TryParse(Get(args, "rup"), out var rup)) return Err("Missing/invalid --rup");
                if (!float.TryParse(Get(args, "rdn"), out var rdn)) return Err("Missing/invalid --rdn");
                return CmdSetRamp(slot, chStr, rup, rdn);
            case "ramp-to":
                if (!float.TryParse(Get(args, "voltage"), out var vT)) return Err("Missing/invalid --voltage");
                float tol  = float.TryParse(Get(args,"tol","5"), out var t1) ? t1 : 5f;
                int   tmo  = int.TryParse(Get(args,"timeout-ms","600000"), out var t2) ? t2 : 600000;
                int   poll = int.TryParse(Get(args,"poll-ms","500"), out var t3) ? t3 : 500;
                return CmdRampTo(slot, chStr, vT, tol, tmo, poll);
            case "ramp-down":
                float tolD  = float.TryParse(Get(args,"tol","5"), out var td1) ? td1 : 5f;
                int   tmoD  = int.TryParse(Get(args,"timeout-ms","600000"), out var td2) ? td2 : 600000;
                int   pollD = int.TryParse(Get(args,"poll-ms","500"), out var td3) ? td3 : 500;
                return CmdRampDownAndOff(slot, chStr, tolD, tmoD, pollD);
            case "param-get":
                return CmdParamGet(slot, chStr, Get(args, "param"));
            case "param-set":
                return CmdParamSet(slot, chStr, Get(args, "param"), Get(args, "value"));
            default:
                return Err($"Unknown command: {cmd}");
        }
    }
}
