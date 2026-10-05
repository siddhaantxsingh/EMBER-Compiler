fn map(xs: [Int], f: fn(Int) -> Int) -> [Int] {
    var out: [Int] = [];
    for x in xs {
        push(out, f(x));
    }
    return out;
}

fn filter(xs: [Int], keep: fn(Int) -> Bool) -> [Int] {
    var out: [Int] = [];
    for x in xs {
        if keep(x) { push(out, x); }
    }
    return out;
}

fn fold(xs: [Int], init: Int, f: fn(Int, Int) -> Int) -> Int {
    var acc = init;
    for x in xs {
        acc = f(acc, x);
    }
    return acc;
}

fn main() {
    let xs = [1, 2, 3, 4, 5, 6];
    let squares = map(xs, fn(x: Int) -> Int { return x * x; });
    print(squares);
    let evens = filter(squares, fn(x: Int) -> Bool { return x % 2 == 0; });
    print(evens);
    print(fold(evens, 0, fn(a: Int, b: Int) -> Int { return a + b; }));
}
