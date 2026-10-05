fn r(n: Int) -> Int {
    if n == 0 { return 1 / n; }
    return r(n - 1);
}

fn main() {
    print(r(30));
}
