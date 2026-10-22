# bench.py — те же тесты на CPython для сравнения с nlang
import time

def arith(n):
    t0 = time.time()
    acc = 0
    i = 0
    while i < n:
        acc += i * 2 - i // 3
        i += 1
    print("  arith acc=" + str(acc))
    return (time.time() - t0) * 1000

def calls(n):
    t0 = time.time()
    acc = 0
    i = 0
    while i < n:
        acc += 1
        i += 1
    return (time.time() - t0) * 1000

def fibo(n):
    if n < 2:
        return n
    return fibo(n - 1) + fibo(n - 2)

def strcat(n):
    t0 = time.time()
    s = ""
    i = 0
    while i < n:
        s += "x"
        i += 1
    print("  strcat len=" + str(len(s)))
    return (time.time() - t0) * 1000

def lists(n):
    t0 = time.time()
    v = []
    i = 0
    while i < n:
        v.append(i)
        i += 1
    acc = 0
    for x in v:
        acc += x
    print("  lists acc=" + str(acc))
    return (time.time() - t0) * 1000

def hotloop(n):
    acc = 0
    i = 0
    while i < n:
        acc += i
        i += 1
    return acc

print("== python bench ==")
print("arith(1e6): %.0f ms" % arith(1000000))
print("calls(1e6): %.0f ms" % calls(1000000))
tf = time.time()
f = fibo(28)
print("fibo(28)=%d: %.0f ms" % (f, (time.time() - tf) * 1000))
print("strcat(1e5): %.0f ms" % strcat(100000))
print("lists(1e6): %.0f ms" % lists(1000000))
th = time.time()
r = 0
while r < 80:
    hotloop(20000)
    r += 1
print("hotloop x80(2e4): %.0f ms" % ((time.time() - th) * 1000))
