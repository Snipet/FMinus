#!/usr/bin/env python3
"""Generates the FMinus benchmark programs (bench_*.fm) for the report's cost tables: the two 1-port blocks (example 1,
DiodeTone; example 2 block 2, the TS clip) as a circuit compiler would emit them, in every variant the ranking compares
(solver and guess, ADAA, oversampling factor, FIR or IIR halfbands, float or float2), and the TS chain partitioned or
not. Every bench function is float f(float n): n host samples of a 1 kHz sine, returning a checksum."""
import sys

coefs = {}
for line in open("hb-coefs.txt"):
    t = line.split()
    coefs[(t[0], t[1])] = [float(x) for x in t[2:]]

def lit(v):
    s = "%.9g" % v
    if "." not in s and "e" not in s:
        s += ".0"
    return s + "f"

# ---------------------------------------------------------------- halfbands, for T = float or float2
def fir(stage, T):
    h2 = coefs[("FIR", stage)]          # 2h of the odd taps, i = 0..J-1
    J = len(h2); W = 2 * J
    n = "Fir%s%s" % (stage, "x2" if T == "float2" else "")
    o = []
    o.append("module %sUp\n{\n    %s d[%d];\n    int pos;\n    void clear()\n    {\n        pos = 0;\n        for (int i = 0; i < %d; ++i)\n            d[i] = 0;\n    }" % (n, T, 2 * W, 2 * W))
    o.append("    void up(%s x, %s* restrict o)\n    {\n        pos = pos == 0 ? %d : pos - 1;\n        d[pos] = x;\n        d[pos + %d] = x;\n        const int p = pos;\n        o[0] = d[p + %d];" % (T, T, W - 1, W, J))
    terms = ["%s * (d[p + %d] + d[p + %d])" % (lit(h2[i]), J + i, J - i - 1) for i in range(J)]
    o.append("        o[1] = " + "\n             + ".join(terms) + ";\n    }\n};\n")
    o.append("module %sDown\n{\n    %s e[%d];\n    %s od[%d];\n    int pe, po;\n    void clear()\n    {\n        pe = 0;\n        po = 0;\n        for (int i = 0; i < %d; ++i)\n            e[i] = 0;\n        for (int i = 0; i < %d; ++i)\n            od[i] = 0;\n    }" % (n, T, 2 * (J + 1), T, 2 * W, 2 * (J + 1), 2 * W))
    o.append("    %s down(%s v0, %s v1)\n    {\n        pe = pe == 0 ? %d : pe - 1;\n        e[pe] = v0;\n        e[pe + %d] = v0;\n        const int q = po;" % (T, T, T, J, J + 1))
    terms = ["%s * (od[q + %d] + od[q + %d])" % (lit(0.5 * h2[i]), J + i, J - i - 1) for i in range(J)]
    o.append("        const %s y = 0.5f * e[pe + %d]\n                + " % (T, J) + "\n                + ".join(terms) + ";")
    o.append("        po = po == 0 ? %d : po - 1;\n        od[po] = v1;\n        od[po + %d] = v1;\n        return y;\n    }\n};\n" % (W - 1, W))
    return "\n".join(o)

def iir(stage, T):
    a = coefs[("IIR", stage)]
    nc = len(a)
    n = "Iir%s%s" % (stage, "x2" if T == "float2" else "")
    out = []
    for d in ("Up", "Down"):
        o = ["module %s%s\n{\n    %s sx[%d];\n    %s sy[%d];\n    void clear()\n    {\n        for (int i = 0; i < %d; ++i)\n        {\n            sx[i] = 0;\n            sy[i] = 0;\n        }\n    }" % (n, d, T, nc, T, nc, nc)]
        if d == "Up":
            o.append("    void up(%s x, %s* restrict o)\n    {\n        %s s0 = x;\n        %s s1 = x;" % (T, T, T, T))
        else:
            o.append("    %s down(%s v0, %s v1)\n    {\n        %s s0 = v1;\n        %s s1 = v0;" % (T, T, T, T, T))
        for i in range(nc):
            sv = "s0" if i % 2 == 0 else "s1"
            o.append("        {\n            const %s t = %s;\n            %s = (%s - sy[%d]) * %s + sx[%d];\n            sx[%d] = t;\n            sy[%d] = %s;\n        }" % (T, sv, sv, sv, i, lit(a[i]), i, i, i, sv))
        if d == "Up":
            o.append("        o[0] = s0;\n        o[1] = s1;\n    }\n};\n")
        else:
            o.append("        return 0.5f * (s0 + s1);\n    }\n};\n")
        out.append("\n".join(o))
    return "\n".join(out)

# ---------------------------------------------------------------- the 1-port solve
NVT = 0.0453
IVT = 1 / NVT

def omega(T):
    return """        const {T} q = fabsf(p);
        const {T} arg = (q + ka) * {ivt} + lna;
        const {T} w = arg <= -3.34145955f ? 0
                    : arg < 8 ? ((-1.31429315e-3f * arg + 4.77593136e-2f) * arg + 3.63195266e-1f) * arg + 6.31318346e-1f
                              : arg - logf(fmaxf(arg, 8));
        {T} v = q + ka - {nvt} * w;
        v = p < 0 ? -v : v;
""".format(T=T, ivt=lit(IVT), nvt=lit(NVT))

def newton(T, K):
    s = "        const %s lo = fminf(p, 0);\n        const %s hi = fmaxf(p, 0);\n" % (T, T)
    for _ in range(K):
        s += """        {{
            const {T} e = expf(v * {ivt});
            const {T} ei = 1 / e;
            const {T} f = v - p + ka * (e - ei);
            const {T} d = 1 + kvth * (e + ei);
            v = fminf(fmaxf(v - f / d, lo), hi);
        }}
""".format(T=T, ivt=lit(IVT))
    return s

def solve(T, guess, K):
    if guess == "w":
        return omega(T) + newton(T, K)
    # previous sample's solution, kept in bracket
    return "        %s v = vprev;\n" % T + newton(T, K).replace("        const %s hi = fmaxf(p, 0);\n" % T, "        const %s hi = fmaxf(p, 0);\n        v = fminf(fmaxf(v, lo), hi);\n" % T) + "        vprev = v;\n"

def adaa_tail(T):
    return """        const {T} ec = expf(v * {ivt});
        const {T} c = ec + 1 / ec;                       // 2 cosh(v / N Vt)
        const {T} dpp = p - p1;
        const {T} vm = 0.5f * (v + v1);
        const {T} vb = fabsf(dpp) > 1e-5f ? vm + ((v - v1) * (0.5f * (p + p1) - vm) - kvt2 * (c - c1)) / dpp : vm;
""".format(T=T, ivt=lit(IVT))

# ---------------------------------------------------------------- example 1 (DiodeTone)
def dt_module(name, T, guess, K, adaa):
    f = ["x1", "x2"]
    if adaa:
        f += ["x1o", "x2o", "u1", "p1", "v1", "c1"]
    if guess == "p":
        f += ["vprev"]
    fields = "    %s %s;\n" % (T, ", ".join(f))
    fields += "    float rate, cp0, cp1, dp, ka, kvth, lna, kvt2;\n"
    if adaa:
        fields += "    float cy0, cy1, dy, wy, a10, a11, b1, w0, w1;\n"
    else:
        fields += "    float gk, din, dx1, dx2, di, gc1x2, gc2x2;\n"
    clear = "\n".join("        %s = 0;" % x for x in f) + ("\n        c1 = 2;" if adaa else "")
    design = "rate * 0.666666667f" if adaa else "rate"
    s = """module {name}
{{
{fields}
    void init(float r)
    {{
        rate = r;
        clear();
        set(0.5f);
    }}
    void clear()
    {{
{clear}
    }}
    void set(float tone)
    {{
        const float fs = {design};
        const float g1 = 1.0f / 2200.0f;
        const float gc1 = 2.0f * 10e-9f * fs;
        const float gc2 = 2.0f * 22e-9f * fs;
        const float t = fminf(fmaxf(tone, 0), 1);
        const float gs = 1.0f / (470.0f + 25000.0f * (1 - t));
        const float s11 = g1 + gc1 + gs;
        const float s22 = gs + gc2;
        const float det = s11 * s22 - gs * gs;
        const float inv = 1.0f / det;
        const float kk = s22 * inv;
        cp0 = s22 * inv;
        cp1 = gs * inv;
        dp = s22 * g1 * inv;
        const float k = 5.04e-9f * kk;
        ka = 0.5f * k;
        kvth = 0.5f * k * {ivt};
        lna = logf(ka * {ivt});
        kvt2 = 0.5f * k * {nvt};
""".format(name=name, fields=fields, clear=clear, design=design, ivt=lit(IVT), nvt=lit(NVT))
    if adaa:
        s += """        const float gkdi = gs / s22;
        cy0 = gs * inv - gkdi * cp0;
        cy1 = s11 * inv - gkdi * cp1;
        dy = gs * g1 * inv - gkdi * dp;
        wy = gkdi;
        a10 = 2 * gc2 * cy0;
        a11 = 2 * gc2 * cy1 - 1;
        b1 = 2 * gc2 * dy;
        w0 = 2 * gc1;
        w1 = 2 * gc2 * wy;
    }
"""
    else:
        s += """        gk = det / s22;
        din = gs * g1 * inv;
        dx1 = gs * inv;
        dx2 = s11 * inv;
        di = gs * inv;
        gc1x2 = 2 * gc1;
        gc2x2 = 2 * gc2;
    }
"""
    s += "    %s step(%s u)\n    {\n        const %s p = cp0 * x1 + cp1 * x2 + dp * u;\n" % (T, T, T)
    s += solve(T, guess, K)
    if adaa:
        s += adaa_tail(T)
        s += """        const {T} xa1 = 0.5f * (x1 + x1o);
        const {T} xa2 = 0.5f * (x2 + x2o);
        const {T} ua = 0.5f * (u + u1);
        const {T} y = cy0 * xa1 + cy1 * xa2 + dy * ua + wy * vb;
        const {T} n1 = w0 * vb - xa1;
        const {T} n2 = a10 * xa1 + a11 * xa2 + b1 * ua + w1 * vb;
        x1o = x1;
        x2o = x2;
        x1 = n1;
        x2 = n2;
        u1 = u;
        p1 = p;
        v1 = v;
        c1 = c;
        return y;
    }}
}};
""".format(T=T)
    else:
        s += """        const {T} i = (p - v) * gk;
        const {T} b = din * u + dx1 * x1 + dx2 * x2 - di * i;
        x1 = gc1x2 * v - x1;
        x2 = gc2x2 * b - x2;
        return b;
    }}
}};
""".format(T=T)
    return s

# ---------------------------------------------------------------- example 2 block 2 (TS clip)
def ts_module(name, T, guess, K, adaa):
    f = ["x3", "x4"]
    if adaa:
        f += ["x3o", "x4o", "u1", "p1", "v1", "c1"]
    if guess == "p":
        f += ["vprev"]
    fields = "    %s %s;\n" % (T, ", ".join(f))
    fields += "    float rate, ka, kvth, lna, kvt2;\n"
    fields += "    float cp0, cp1, dp, a00, b0, w1;\n" if adaa else "    float g4, inA, invB, gc3x2, gc4x2;\n"
    clear = "\n".join("        %s = 0;" % x for x in f) + ("\n        c1 = 2;" if adaa else "")
    design = "rate * 0.666666667f" if adaa else "rate"
    s = """module {name}
{{
{fields}
    void init(float r)
    {{
        rate = r;
        clear();
        set(1.0f);
    }}
    void clear()
    {{
{clear}
    }}
    void set(float drive)
    {{
        const float fs = {design};
        const float d = fminf(fmaxf(drive, 0), 1);
        const float g4_ = 1.0f / 4700.0f;
        const float gc3 = 2.0f * 47e-9f * fs;
        const float gc4 = 2.0f * 51e-12f * fs;
        const float inA_ = 1.0f / (g4_ + gc3);
        const float gf = 1.0f / (51000.0f + 500000.0f * (expf(4.39444915f * d) - 1) * 0.0125f);
        const float invB_ = 1.0f / (gf + gc4);
        const float k = 5.04e-9f * invB_;
        ka = 0.5f * k;
        kvth = 0.5f * k * {ivt};
        lna = logf(ka * {ivt});
        kvt2 = 0.5f * k * {nvt};
""".format(name=name, fields=fields, clear=clear, design=design, ivt=lit(IVT), nvt=lit(NVT))
    if adaa:
        s += """        cp0 = -g4_ * inA_ * invB_;
        cp1 = invB_;
        dp = (g4_ - g4_ * g4_ * inA_) * invB_;
        a00 = 2 * gc3 * inA_ - 1;
        b0 = 2 * gc3 * inA_ * g4_;
        w1 = 2 * gc4;
    }
"""
        s += "    %s step(%s u)\n    {\n        const %s p = cp0 * x3 + cp1 * x4 + dp * u;\n" % (T, T, T)
    else:
        s += """        g4 = g4_;
        inA = inA_;
        invB = invB_;
        gc3x2 = 2 * gc3;
        gc4x2 = 2 * gc4;
    }
"""
        s += "    %s step(%s u)\n    {\n        const %s va = (u * g4 + x3) * inA;\n        const %s p = ((u - va) * g4 + x4) * invB;\n" % (T, T, T, T)
    s += solve(T, guess, K)
    if adaa:
        s += adaa_tail(T)
        s += """        const {T} xa3 = 0.5f * (x3 + x3o);
        const {T} xa4 = 0.5f * (x4 + x4o);
        const {T} ua = 0.5f * (u + u1);
        const {T} n3 = a00 * xa3 + b0 * ua;
        const {T} n4 = w1 * vb - xa4;
        x3o = x3;
        x4o = x4;
        x3 = n3;
        x4 = n4;
        u1 = u;
        p1 = p;
        v1 = v;
        c1 = c;
        return ua + vb;
    }}
}};
""".format(T=T)
    else:
        s += """        x3 = gc3x2 * va - x3;
        x4 = gc4x2 * v - x4;
        return u + v;
    }}
}};
""".format(T=T)
    return s

# TS blocks 1, 3, 4 (linear)
TS_LIN = """module TsB1                         // C1 1u in -> p1, R1 10k p1 -> 0
{
    float h, gc, inv;
    void init(float rate)
    {
        gc = 2.0f * 1e-6f * rate;
        inv = 1.0f / (gc + 1.0f / 10000.0f);
        h = 0;
    }
    float step(float u)
    {
        const float vp = (gc * u - h) * inv;
        h = 2 * gc * (u - vp) - h;
        return vp;
    }
};

module TsB3                         // the tone stage (Yeh DAFx-07 fig. 15, eq. 24): Rs 1k, Cs 0.22u, Ri 10k, pot 20k, Rz 220, Cz 0.22u, Rf 1k
{
    float h5, h6, g5x2, g6x2, c1, c2, c3, al, be, m, iz, o1, o2, gz, g5, g6;
    void init(float rate)
    {
        g5 = 2.0f * 0.22e-6f * rate;
        g6 = 2.0f * 0.22e-6f * rate;
        g5x2 = 2 * g5;
        g6x2 = 2 * g6;
        gz = 1.0f / 220.0f;
        h5 = 0;
        h6 = 0;
        set(0.5f);
    }
    void set(float tone)            // T = tone: Rl (+ input side) = T 20k, Rr (- input side) = (1 - T) 20k, kept off 0 ohm
    {
        const float t = fminf(fmaxf(tone, 0.0005f), 0.9995f);
        const float ga = 1.0f / (t * 20000.0f);
        const float gb = 1.0f / ((1 - t) * 20000.0f);
        m = gz / (gz + g6);
        iz = 1.0f / (gz + g6);
        const float d2 = ga + gb + gz * (1 - m);
        al = (ga + gb) / d2;
        be = m / d2;
        const float gs = 1.0f / 1000.0f;
        const float den = 1.0f / (gs + 1.0f / 10000.0f + g5 + ga - ga * al);
        c1 = gs * den;
        c2 = den;
        c3 = ga * be * den;
        o1 = 1 + 1000.0f * gb;
        o2 = 1000.0f * gb;
    }
    float step(float u)
    {
        const float vp = c1 * u + c2 * h5 + c3 * h6;
        const float vw = al * vp + be * h6;
        const float vz = m * vw + iz * h6;
        h5 = g5x2 * vp - h5;
        h6 = g6x2 * vz - h6;
        return o1 * vp - o2 * vw;
    }
};

module TsB4                         // C7 1u v3 -> o1, R11 1k + level pot 100k to 0; out at the wiper
{
    float h, gc, inv, gt, gout;
    void init(float rate)
    {
        gc = 2.0f * 1e-6f * rate;
        gt = 1.0f / 101000.0f;
        inv = 1.0f / (gc + gt);
        h = 0;
        set(0.7f);
    }
    void set(float level)
    {
        gout = gt * 100000.0f * fminf(fmaxf(level, 0), 1);
    }
    float step(float u)
    {
        const float va = (gc * u - h) * inv;
        h = 2 * gc * (u - va) - h;
        return gout * va;
    }
};
"""

# ---------------------------------------------------------------- oversampled wrappers
def os_chain(stages, kind, T):
    """Returns (fields, init_lines, up_code(x) -> list of high-rate exprs, down_code(list) -> expr) for 2^len(stages)x."""
    sfx = "x2" if T == "float2" else ""
    fields, init = [], []
    for st in stages:
        fields.append("    %s%s%sUp u%s;\n    %s%s%sDown d%s;\n" % (kind, st, sfx, st, kind, st, sfx, st))
        init.append("        u%s.clear();\n        d%s.clear();\n" % (st, st))
    return "".join(fields), "".join(init)

def wrapper(name, T, block, M, kind, linear_ts=None, all_fast=False):
    """block: module name of the 1-port block. M in 1, 2, 4, 8. kind: Fir or Iir. linear_ts: None, 'part' (blocks 1,3,4
    at 1x around the oversampled clip) or 'all' (every block at M x)."""
    stages = ["S1", "S2", "S3"][: {1: 0, 2: 1, 4: 2, 8: 3}[M]]
    f, init = os_chain(stages, kind, T)
    nb = M
    arrs = ""
    for lvl in range(len(stages)):
        cnt = 2 ** lvl
        for j in range(cnt):
            arrs += "    %s a%d_%d[2];\n" % (T, lvl, j)
    s = "module %s\n{\n    %s c;\n%s%s" % (name, block, f, arrs)
    if linear_ts:
        s += "    TsB1 b1;\n    TsB3 b3;\n    TsB4 b4;\n"
    s += "    void init(float rate)\n    {\n        c.init(%d * rate);\n%s" % (M, init)
    if linear_ts:
        r = "rate" if linear_ts == "part" else "%d * rate" % M
        s += "        b1.init(%s);\n        b3.init(%s);\n        b4.init(%s);\n" % (r, r, r)
    s += "    }\n"
    s += "    %s tick(%s x)\n    {\n" % (T, T)
    inner = lambda e: e
    if linear_ts == "part":
        s += "        x = b1.step(x);\n"
    def stepexpr(e):
        if linear_ts == "all":
            return "b4.step(b3.step(c.step(b1.step(%s))))" % e
        return "c.step(%s)" % e
    # up
    cur = ["x"]
    for lvl, st in enumerate(stages):
        nxt = []
        for j, e in enumerate(cur):
            s += "        u%s.up(%s, a%d_%d);\n" % (st, e, lvl, j)
            nxt += ["a%d_%d[0]" % (lvl, j), "a%d_%d[1]" % (lvl, j)]
        cur = nxt
    ys = []
    for j, e in enumerate(cur):
        s += "        const %s y%d = %s;\n" % (T, j, stepexpr(e))
        ys.append("y%d" % j)
    for st in reversed(stages):
        nxt = []
        for j in range(0, len(ys), 2):
            s += "        const %s z%s_%d = d%s.down(%s, %s);\n" % (T, st, j // 2, st, ys[j], ys[j + 1])
            nxt.append("z%s_%d" % (st, j // 2))
        ys = nxt
    out = ys[0]
    if linear_ts == "part":
        out = "b4.step(b3.step(%s))" % out
    s += "        return %s;\n    }\n};\n" % out
    return s

def bench(fn, wrap, T, amp):
    if T == "float":
        x = "%s * sinf(ph)" % lit(amp)
        acc = "const float y = w.tick(x);\n        sum += y * y;"
        ret = "sum"
    else:
        x = "(float2)(%s * sinf(ph), %s * cosf(ph))" % (lit(amp), lit(amp))
        acc = "const float2 y = w.tick(x);\n        sum += y.x * y.x + y.y * y.y;"
        ret = "sum"
    return """float {fn}(float n)
{{
    {wrap} w;
    w.init(48000);
    float sum = 0;
    float ph = 0;
    for (int i = 0; i < (int)n; ++i)
    {{
        const {T} x = {x};
        ph += 0.130899694f;
        ph = ph > 6.28318531f ? ph - 6.28318531f : ph;
        {acc}
    }}
    return {ret};
}}
""".format(fn=fn, wrap=wrap, T=T, x=x, acc=acc, ret=ret)

def baseline(T):
    if T == "float":
        x = "4.5f * sinf(ph)"
        body = "sum += x * x;"
    else:
        x = "(float2)(4.5f * sinf(ph), 4.5f * cosf(ph))"
        body = "sum += x.x * x.x + x.y * x.y;"
    return """float baseline_{T}(float n)
{{
    float sum = 0;
    float ph = 0;
    for (int i = 0; i < (int)n; ++i)
    {{
        const {T} x = {x};
        ph += 0.130899694f;
        ph = ph > 6.28318531f ? ph - 6.28318531f : ph;
        {body}
    }}
    return sum;
}}
""".format(T=T, x=x, body=body)

if __name__ == "__main__":
    which = sys.argv[1]
    out = []
    names = []
    if which in ("dt", "ts", "tsfull", "dt2"):
        T = "float2" if which == "dt2" else "float"
        for st in ("S1", "S2", "S3"):
            out.append(fir(st, T))
            out.append(iir(st, T))
        out.append(baseline(T))
        names.append("baseline_" + T)
    if which == "dt" or which == "dt2":
        T = "float2" if which == "dt2" else "float"
        variants = [("w", 1, False), ("w", 2, False), ("p", 2, False), ("p", 4, False), ("w", 1, True)]
        for (g, K, ad) in variants:
            mod = "DT_%s%d%s" % (g, K, "_adaa" if ad else "")
            out.append(dt_module(mod, T, g, K, ad))
            for M in (1, 2, 4, 8):
                for kind in ("Fir", "Iir"):
                    if kind == "Iir" and not (g == "w" and K in (1, 2)) and not ad:
                        continue
                    wn = "W_%s_%dx_%s" % (mod, M, kind)
                    out.append(wrapper(wn, T, mod, M, kind))
                    fn = "b_%s_%dx_%s" % (mod, M, kind.lower())
                    out.append(bench(fn, wn, T, 4.5))
                    names.append(fn)
    if which == "ts":
        T = "float"
        variants = [("w", 1, False), ("w", 2, False), ("p", 4, False), ("p", 6, False), ("w", 1, True)]
        for (g, K, ad) in variants:
            mod = "TS_%s%d%s" % (g, K, "_adaa" if ad else "")
            out.append(ts_module(mod, T, g, K, ad))
            for M in (1, 2, 4, 8):
                for kind in ("Fir", "Iir"):
                    wn = "W_%s_%dx_%s" % (mod, M, kind)
                    out.append(wrapper(wn, T, mod, M, kind))
                    fn = "b_%s_%dx_%s" % (mod, M, kind.lower())
                    out.append(bench(fn, wn, T, 0.45))
                    names.append(fn)
    if which == "tsfull":
        T = "float"
        out.append(TS_LIN)
        for (g, K, ad) in [("w", 2, False), ("w", 1, True)]:
            mod = "TS_%s%d%s" % (g, K, "_adaa" if ad else "")
            out.append(ts_module(mod, T, g, K, ad))
            for M in ((8, 4) if not ad else (4, 2)):
                for kind in ("Fir", "Iir"):
                    for lin in ("part", "all"):
                        wn = "WF_%s_%dx_%s_%s" % (mod, M, kind, lin)
                        out.append(wrapper(wn, T, mod, M, kind, linear_ts=lin))
                        fn = "f_%s_%dx_%s_%s" % (mod, M, kind.lower(), lin)
                        out.append(bench(fn, wn, T, 0.45))
                        names.append(fn)
    open("bench_%s.fm" % which, "w").write("\n".join(out))
    open("bench_%s.names" % which, "w").write(" ".join(names))
    print(len(names), "bench functions")
