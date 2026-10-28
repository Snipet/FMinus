#!/usr/bin/env python3
# Scripts/fit-math-kernels.py: fits the polynomials of src/runtime/MathKernels.h again and prints them as its C++ arrays
# (docs/extending.md, recipe 23). Python 3 alone: the functions and the arithmetic are decimal's, to 60 digits.
#
# Each polynomial is a minimax fit (Remez's exchange) of a function's remainder, weighted so that the error it
# minimises is the kernel's relative error, then given float coefficients: one at a time, lowest degree first, each
# rounded and the rest fitted again around it ("refit"), or all rounded at once ("plain", for log, whose weight would
# vanish inside its interval). It prints each fit's largest weighted error with float coefficients, then the arrays.
import math
import struct
from decimal import Decimal as D, getcontext

getcontext().prec = 60


def pi():
    # Machin's formula: pi = 16 atan(1/5) - 4 atan(1/239).
    getcontext().prec += 10

    def atan_inverse(n):
        x = D(1) / n
        term, total, k = x, x, 1
        while True:
            term *= -x * x
            k += 2
            if abs(term / k) < D(10) ** -(getcontext().prec + 2):
                break
            total += term / k
        return total

    value = 16 * atan_inverse(5) - 4 * atan_inverse(239)
    getcontext().prec -= 10
    return +value


PI = pi()


def series(x, first, step):
    # sin (first = x) and cos (first = 1) by their Taylor series, for |x| <= pi/2.
    x = D(x)
    term, total, k = first, first, 0 if first == 1 else 1
    while True:
        term = -term * x * x / ((k + 1) * (k + 2))
        k += 2
        if abs(term) < D(10) ** -70:
            return total
        total += term


def sin(x):
    return series(x, D(x), 2)


def cos(x):
    return series(x, D(1), 2)


def expm1(x):
    x = D(x)
    if abs(x) > D("0.01"):
        return x.exp() - 1
    term, total, k = x, x, 1
    while abs(term) >= D(10) ** -75:
        k += 1
        term = term * x / k
        total += term
    return total


def log1p(f):
    f = D(f)
    if abs(f) > D("0.01"):
        return (1 + f).ln()
    power, total, k = f, f, 1
    while True:
        k += 1
        power = -power * f
        if abs(power / k) < D(10) ** -75:
            return total
        total += power / k


def tanh(x):
    e = expm1(2 * D(x))
    return e / (e + 2)


def to_float(x):
    return struct.unpack("f", struct.pack("f", float(x)))[0]


def hex_float(x):
    # A float as C++ writes it: 0x1.555546p-3f, -0x1p-1f.
    mantissa, exponent = float(x).hex().split("p")
    mantissa = mantissa.rstrip("0").rstrip(".")
    return f"{mantissa}p{exponent}f"


def power(t, p):
    return D(1) if p == 0 else t**p


def solve(rows, rhs):
    n = len(rows)
    m = [row[:] + [b] for row, b in zip(rows, rhs)]
    for c in range(n):
        pivot = max(range(c, n), key=lambda r: abs(m[r][c]))
        m[c], m[pivot] = m[pivot], m[c]
        for r in range(n):
            if r != c and m[r][c] != 0:
                factor = m[r][c] / m[c][c]
                for k in range(c, n + 1):
                    m[r][k] -= factor * m[c][k]
    return [m[i][n] / m[i][i] for i in range(n)]


def remez(g, w, a, b, degrees, fixed, start=None, iterations=30, grid=3000):
    """The coefficients of sum c_j t^j (j < degrees) minimising max |w(t) (g(t) - sum)| on [a, b], those in `fixed`
    kept, and the final reference."""
    free = [j for j in range(degrees) if j not in fixed]
    a, b = D(a), D(b)
    n = len(free)
    eps = (b - a) * D("1e-9")

    def away(t):   # no reference point where the weight vanishes
        return t + eps if w(t) == 0 else t

    reference = [away((a + b) / 2 - (b - a) / 2 * D(math.cos(math.pi * i / n))) for i in range(n + 1)]
    # Where the weight is small (r^2 Q's relative error near r = 0), start from where it is at least a tenth of its
    # largest.
    probe = [a + (b - a) * i / 400 for i in range(401)]
    weights = [w(away(t)) for t in probe]
    good = [t for t, x in zip(probe, weights) if x >= max(weights) / 10]
    if len(good) < len(probe):
        candidates = [good[0] + (good[-1] - good[0]) * i / 2000 for i in range(2001)]
        candidates = [t for t in candidates if w(away(t)) >= max(weights) / 10]
        reference = [candidates[round(i * (len(candidates) - 1) / n)] for i in range(n + 1)]
    if start is not None and len(start) >= n + 1:
        reference = list(start)
        while len(reference) > n + 1:
            reference.pop(len(reference) // 2)
    points = [away(a + (b - a) * i / grid) for i in range(grid + 1)]
    values = [g(t) for t in points]
    pointWeights = [w(t) for t in points]
    c = {}
    for _ in range(iterations):
        rows, rhs = [], []
        for i, t in enumerate(reference):
            rows.append([power(t, j) for j in free] + [D((-1) ** i) / w(t)])
            rhs.append(g(t) - sum(fixed[j] * power(t, j) for j in fixed))
        solution = solve(rows, rhs)
        c = dict(zip(free, solution))
        c.update(fixed)
        level = solution[-1]
        errors = [x * (v - sum(c[j] * power(t, j) for j in range(degrees)))
                  for t, v, x in zip(points, values, pointWeights)]
        extrema = []
        for i, e in enumerate(errors):
            left = errors[i - 1] if i > 0 else None
            right = errors[i + 1] if i + 1 < len(errors) else None
            if (left is None or abs(e) >= abs(left)) and (right is None or abs(e) >= abs(right)):
                extrema.append((points[i], e))
        alternating = []
        for t, e in extrema:
            if alternating and (alternating[-1][1] > 0) == (e > 0):
                if abs(e) > abs(alternating[-1][1]):
                    alternating[-1] = (t, e)
            else:
                alternating.append((t, e))
        while len(alternating) > n + 1:
            alternating.pop(0 if abs(alternating[0][1]) < abs(alternating[-1][1]) else -1)
        largest = max(abs(e) for e in errors)
        if len(alternating) < n + 1:
            break
        reference = [t for t, _ in alternating]
        if abs(largest - abs(level)) / largest < D("1e-6"):
            break
    return [c[j] for j in range(degrees)], reference


def fit(g, w, a, b, degrees, rounding):
    c, reference = remez(g, w, a, b, degrees, {})
    if rounding == "refit":
        fixed = {}
        for j in range(degrees):
            fixed[j] = D(to_float(c[j]))
            if len(fixed) < degrees:
                c, reference = remez(g, w, a, b, degrees, dict(fixed), start=reference)
                c = [fixed.get(k, c[k]) for k in range(degrees)]
    floats = [D(to_float(x)) for x in c]
    largest = max(abs(w(t) * (g(t) - sum(floats[j] * power(t, j) for j in range(degrees))))
                  for t in [D(a) + (D(b) - D(a)) * i / 6000 for i in range(6001)])
    return floats, largest


def main():
    quarter = (PI / 4) ** 2 * (1 + D("1e-6"))
    half_ln2 = D(2).ln() / 2 * (1 + D("1e-6"))
    tanh_small = D("0.5493061443")   # atanh(1/2)
    fits = [
        # sin(r) = r + r^3 P(r^2), cos(r) = 1 + r^2 Q(r^2), r in [-pi/4, pi/4]; relative errors
        ("kSinP", lambda z: (sin(z.sqrt()) - z.sqrt()) / (z * z.sqrt()) if z else D(-1) / 6,
         lambda z: z * z.sqrt() / sin(z.sqrt()) if z else D(0), 0, quarter, 3, "refit"),
        ("kCosP", lambda z: (cos(z.sqrt()) - 1) / z if z else D(-1) / 2, lambda z: z / cos(z.sqrt()), 0, quarter, 4,
         "refit"),
        # e^r = 1 + r + r^2 Q(r), r in [-ln2/2, ln2/2]
        ("kExpQ", lambda r: (expm1(r) - r) / (r * r) if r else D(1) / 2, lambda r: r * r / r.exp(), -half_ln2,
         half_ln2, 5, "refit"),
        # log(1 + f) = f + f^2 P(f), f in [sqrt(1/2) - 1, sqrt(2) - 1]: P's own error, which the kernel scales by f
        ("kLogP", lambda f: (log1p(f) - f) / (f * f) if f else D(-1) / 2, lambda f: D(1),
         (D("0.5").sqrt() - 1) * (1 + D("1e-6")), (D(2).sqrt() - 1) * (1 + D("1e-6")), 10, "plain"),
        # tanh(x) = x + x^3 T(x^2), x in [0, atanh(1/2)]
        ("kTanhT", lambda z: (tanh(z.sqrt()) - z.sqrt()) / (z * z.sqrt()) if z else D(-1) / 3,
         lambda z: z * z.sqrt() / tanh(z.sqrt()) if z else D(0), 0, tanh_small * tanh_small * (1 + D("1e-6")), 5,
         "refit"),
    ]
    arrays = []
    for name, g, w, a, b, degrees, rounding in fits:
        floats, largest = fit(g, w, a, b, degrees, rounding)
        print(f"// {name}: largest weighted error {float(largest):.3e} (2^{math.log2(float(largest)):.2f})")
        arrays.append(f"inline constexpr float {name}[] = {{{', '.join(hex_float(x) for x in floats)}}};")
    for array in arrays:
        print(array)


if __name__ == "__main__":
    main()
