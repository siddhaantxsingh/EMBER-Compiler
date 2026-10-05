fn fib(n: Int) -> Int {
    if n < 2 { return n; }
    return fib(n - 1) + fib(n - 2);
}

fn ackermann(m: Int, n: Int) -> Int {
    if m == 0 { return n + 1; }
    if n == 0 { return ackermann(m - 1, 1); }
    return ackermann(m - 1, ackermann(m, n - 1));
}

fn main() {
    print(fib(25));
    print(ackermann(2, 3));
}
