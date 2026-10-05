fn apply(f: fn(Int) -> Int, x: Int) -> Int {
    return f(x);
}

fn main() {
    let inv = fn(x: Int) -> Int { return 100 / x; };
    print(apply(inv, 5));
    print(apply(inv, 0));
}
